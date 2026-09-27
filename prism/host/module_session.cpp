#include "prism/sdk/module_session.hpp"
#include "module_work_p.hpp"
#include "prism/host/event_wait.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace prism::sdk {
std::uint64_t MonotonicNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

namespace {
bool Valid(PrismStringViewV1 text, std::size_t limit)
{
    return text.data && text.size > 0 && text.size <= limit &&
           std::string_view(text.data, text.size).find('\0') == std::string_view::npos;
}
} // namespace

ModuleSession::ModuleSession(const std::filesystem::path &module, std::string app_id,
                             std::uint64_t instance, BindingSink bindings, LaunchSink launch,
                             SubscribeSink subscribe, ThemeSink themes, ColorSchemeSink schemes,
                             std::shared_ptr<runtime::TaskScheduler> scheduler,
                             ModuleSessionLimits limits)
    : module_(module), app_id_(std::move(app_id)), instance_id_(instance),
      bindings_(std::move(bindings)), launch_(std::move(launch)), subscribe_(std::move(subscribe)),
      themes_(std::move(themes)), schemes_(std::move(schemes)),
      host_{sizeof(host_), PRISM_APP_ABI_V1, this,      SetBinding,  Ready,
            Launch,        Schedule,         Subscribe, SelectTheme, SelectColorScheme,
            SubmitWork,    CancelWork},
      limits_(limits), work_(std::make_unique<ModuleWorkState>())
{
    if (!limits_.outstanding || limits_.outstanding > 128 || !limits_.input_bytes ||
        !limits_.result_bytes || !limits_.queued_input_bytes ||
        limits_.queued_input_bytes > 1024 * 1024 || !limits_.completions_per_turn ||
        limits_.dispatch_budget.count() <= 0 || limits_.entry_budget.count() <= 0) {
        throw std::invalid_argument("Business preparation limits must be positive and bounded");
    }
    work_->scheduler =
        scheduler ? std::move(scheduler) : std::make_shared<runtime::TaskScheduler>();
    work_->channel = work_->scheduler->OpenChannel(limits_.outstanding, 2, false);
}

ModuleSession::~ModuleSession()
{
    if (!OnOwnerThread()) {
        std::terminate();
    }
    StopWork();
    DestroyInstance();
    // AppModule unload follows work join and instance destruction.
}

bool ModuleSession::Start()
{
    if (!OnOwnerThread() || closed_ || started_ || !instance_id_ || !bindings_) {
        return false;
    }
    started_ = true;
    const auto budget_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(limits_.entry_budget).count();
    if (module_.LoadDurationNs() > static_cast<std::uint64_t>(budget_ns)) {
        start_diagnostic_ = "Module load entry exceeded its cooperative entry budget";
        StopWork();
        return false;
    }

    PrismAppInitV1 init{
        sizeof(init), PRISM_APP_ABI_V1, instance_id_, {app_id_.data(), app_id_.size()}, &host_};
    const auto start = std::chrono::steady_clock::now();
    try {
        instance_ = module_.Api().create(&init);
    } catch (...) {
        start_diagnostic_ = "Module create threw across the application ABI";
    }
    if (std::chrono::steady_clock::now() - start > limits_.entry_budget) {
        start_diagnostic_ = "Module create exceeded its cooperative entry budget";
    } else if (!instance_ && start_diagnostic_.empty()) {
        start_diagnostic_ = "Module create returned no instance";
    }
    if (!start_diagnostic_.empty()) {
        StopWork();
        DestroyInstance();
        return false;
    }
    return true;
}

bool ModuleSession::OnOwnerThread() const noexcept
{
    return std::this_thread::get_id() == owner_thread_;
}

void ModuleSession::DestroyInstance() noexcept
{
    if (instance_) {
        module_.Api().destroy(instance_);
        instance_ = nullptr;
    }
}

const std::string &ModuleSession::StartDiagnostic() const noexcept
{
    static const std::string empty;
    return OnOwnerThread() ? start_diagnostic_ : empty;
}

int32_t ModuleSession::SetBinding(void *ctx, PrismStringViewV1 key, PrismValueV1 value) noexcept
{
    if (!ctx || !static_cast<ModuleSession *>(ctx)->OnOwnerThread()) {
        return -1;
    }
    try {
        auto &self = *static_cast<ModuleSession *>(ctx);
        if (self.closed_ || !Valid(key, 128)) {
            return -1;
        }
        runtime::PropertyValue converted;
        switch (value.kind) {
        case PRISM_VALUE_STRING_V1:
            if (value.as.string.size > 65536 || (!value.as.string.data && value.as.string.size)) {
                return -1;
            }
            converted = value.as.string.size
                            ? std::string(value.as.string.data, value.as.string.size)
                            : std::string{};
            if (std::get<std::string>(converted).find('\0') != std::string::npos) {
                return -1;
            }
            break;
        case PRISM_VALUE_NUMBER_V1:
            if (!std::isfinite(value.as.number)) {
                return -1;
            }
            converted = value.as.number;
            break;
        case PRISM_VALUE_BOOL_V1:
            if (value.as.boolean > 1) {
                return -1;
            }
            converted = value.as.boolean != 0;
            break;
        case PRISM_VALUE_COLOR_V1:
            converted = contracts::Color{static_cast<uint8_t>(value.as.rgba >> 24),
                                         static_cast<uint8_t>(value.as.rgba >> 16),
                                         static_cast<uint8_t>(value.as.rgba >> 8),
                                         static_cast<uint8_t>(value.as.rgba)};
            break;
        default:
            return -1;
        }
        return self.bindings_(std::string_view(key.data, key.size), std::move(converted)) ? 0 : -1;
    } catch (...) {
        return -1;
    }
}

int32_t ModuleSession::Ready(void *ctx) noexcept
{
    if (!ctx || !static_cast<ModuleSession *>(ctx)->OnOwnerThread()) {
        return -1;
    }
    auto &self = *static_cast<ModuleSession *>(ctx);
    if (self.closed_ || self.ready_) {
        return -1;
    }
    self.ready_ = true;
    return 0;
}

uint64_t ModuleSession::Launch(void *context, PrismStringViewV1 app_id) noexcept
{
    if (!context || !static_cast<ModuleSession *>(context)->OnOwnerThread()) {
        return 0;
    }
    try {
        auto &self = *static_cast<ModuleSession *>(context);
        if (self.closed_ || !Valid(app_id, 128) || !self.launch_ || self.launches_.size() >= 64) {
            return 0;
        }
        const std::string_view name(app_id.data, app_id.size);
        auto id = self.launch_(name);
        if (!id) {
            return 0;
        }
        self.launches_.emplace(id, PendingLaunch{std::string(name), {}, 0, false});
        return id;
    } catch (...) {
        return 0;
    }
}

uint64_t ModuleSession::Subscribe(void *ctx) noexcept
{
    if (!ctx || !static_cast<ModuleSession *>(ctx)->OnOwnerThread()) {
        return 0;
    }
    try {
        auto &self = *static_cast<ModuleSession *>(ctx);
        if (self.closed_) {
            return 0;
        }
        if (self.subscription_) {
            return self.subscription_;
        }
        if (!self.subscribe_) {
            return 0;
        }
        return self.subscription_ = self.subscribe_();
    } catch (...) {
        return 0;
    }
}

uint64_t ModuleSession::SelectTheme(void *ctx, PrismStringViewV1 id) noexcept
{
    if (!ctx || !static_cast<ModuleSession *>(ctx)->OnOwnerThread()) {
        return 0;
    }
    try {
        auto &self = *static_cast<ModuleSession *>(ctx);
        if (self.closed_ || (!id.data && id.size) || id.size > 128 || !self.themes_ ||
            self.theme_requests_.size() >= 64) {
            return 0;
        }
        const auto name = id.size ? std::string_view(id.data, id.size) : std::string_view{};
        if (name.find('\0') != std::string_view::npos) {
            return 0;
        }
        const auto request = self.themes_(name);
        if (!request || self.theme_requests_.contains(request)) {
            return 0;
        }
        self.theme_requests_.emplace(request, name);
        return request;
    } catch (...) {
        return 0;
    }
}

uint64_t ModuleSession::SelectColorScheme(void *ctx, PrismStringViewV1 scheme) noexcept
{
    if (!ctx || !static_cast<ModuleSession *>(ctx)->OnOwnerThread()) {
        return 0;
    }
    try {
        auto &self = *static_cast<ModuleSession *>(ctx);
        if (self.closed_ || !Valid(scheme, 5) || !self.schemes_ ||
            self.theme_requests_.size() >= 64) {
            return 0;
        }
        const std::string_view name(scheme.data, scheme.size);
        if (name != "light" && name != "dark") {
            return 0;
        }
        const auto request = self.schemes_(name);
        if (!request || self.theme_requests_.contains(request)) {
            return 0;
        }
        self.theme_requests_.emplace(request, std::string{});
        return request;
    } catch (...) {
        return 0;
    }
}

void ModuleSession::Deliver(const contracts::ThemeEvent &event)
{
    if (!OnOwnerThread() || closed_) {
        return;
    }
    if (event.request && !theme_requests_.contains(event.request)) {
        return;
    }
    if (instance_ && module_.Api().on_theme_event) {
        PrismThemeEventV1 projected{sizeof(projected),
                                    event.request,
                                    event.generation,
                                    static_cast<std::uint32_t>(event.status),
                                    {event.id.data(), event.id.size()},
                                    {event.name.data(), event.name.size()},
                                    {event.detail.data(), event.detail.size()},
                                    {event.color_scheme.data(), event.color_scheme.size()}};
        module_.Api().on_theme_event(instance_, &projected);
    }
    if (event.request) {
        theme_requests_.erase(event.request);
    }
}

void ModuleSession::Deliver(const contracts::InstanceUpdate &event)
{
    if (!OnOwnerThread() || closed_) {
        return;
    }
    if (!subscription_ || event.request.value != subscription_ || !instance_ ||
        !module_.Api().on_instance_event) {
        return;
    }
    PrismInstanceEventV1 projected{sizeof(projected),
                                   event.request.value,
                                   event.instance.value,
                                   event.pid,
                                   static_cast<std::uint32_t>(event.change),
                                   {event.app_id.data(), event.app_id.size()}};
    module_.Api().on_instance_event(instance_, &projected);
}

int32_t ModuleSession::Schedule(void *ctx, uint64_t delay) noexcept
{
    if (!ctx || !static_cast<ModuleSession *>(ctx)->OnOwnerThread()) {
        return -1;
    }
    auto &self = *static_cast<ModuleSession *>(ctx);
    const auto now = MonotonicNs();
    if (self.closed_ || !self.module_.Api().on_tick ||
        delay > std::numeric_limits<uint64_t>::max() - now) {
        return -1;
    }
    self.tick_due_ = now + delay;
    return 0;
}

void ModuleSession::Action(std::string_view action)
{
    if (!OnOwnerThread() || closed_) {
        return;
    }
    if (instance_ && module_.Api().on_action) {
        module_.Api().on_action(instance_, {action.data(), action.size()});
    }
}

void ModuleSession::Tick(std::uint64_t now)
{
    if (!OnOwnerThread() || closed_) {
        return;
    }
    if (!instance_ || !tick_due_ || now < *tick_due_) {
        return;
    }
    tick_due_.reset(); // A callback may schedule its next tick.
    module_.Api().on_tick(instance_, now);
}

int ModuleSession::TimeoutMs(std::uint64_t now, int maximum) const
{
    if (!OnOwnerThread() || closed_) {
        return maximum;
    }
    return host::Timeout(now, tick_due_, maximum);
}
} // namespace prism::sdk

namespace prism::sdk {
void ModuleSession::Deliver(const contracts::LaunchEvent &event)
{
    if (!OnOwnerThread() || closed_) {
        return;
    }
    auto it = launches_.find(event.request.value);
    if (it == launches_.end()) {
        return;
    }
    auto &pending = it->second;
    pending.instance = event.instance;
    pending.pid = event.pid;
    if (event.milestone == contracts::LaunchMilestone::Failed) {
        pending.failed = true;
    }
    if (instance_ && module_.Api().on_launch_event) {
        PrismLaunchEventV1 projected{sizeof(projected),
                                     event.request.value,
                                     event.instance.value,
                                     event.pid,
                                     static_cast<std::uint32_t>(event.milestone),
                                     static_cast<std::uint32_t>(event.error),
                                     event.exit_code,
                                     {pending.app_id.data(), pending.app_id.size()},
                                     {event.detail.data(), event.detail.size()}};
        module_.Api().on_launch_event(instance_, &projected);
    }
    if (event.milestone == contracts::LaunchMilestone::Activated ||
        event.milestone == contracts::LaunchMilestone::Exited ||
        (event.milestone == contracts::LaunchMilestone::Failed && !event.pid)) {
        launches_.erase(it);
    }
}

void ModuleSession::Disconnected()
{
    if (!OnOwnerThread() || closed_) {
        return;
    }
    if (subscription_) {
        Deliver(contracts::InstanceUpdate{
            {subscription_}, {}, 0, contracts::InstanceChange::Reset, {}});
    }
    auto pending = launches_;
    for (const auto &[id, launch] : pending) {
        if (!launch.failed) {
            Deliver({{id},
                     launch.instance,
                     launch.pid,
                     contracts::LaunchMilestone::Failed,
                     contracts::LaunchError::SessionEnded,
                     0,
                     "Launch service disconnected"});
        }
    }
    launches_.clear();
    const auto pending_themes = theme_requests_;
    for (const auto &[id, name] : pending_themes) {
        Deliver(contracts::ThemeEvent{
            id, 0, contracts::ThemeStatus::Rejected, name, {}, "Theme service disconnected"});
    }
    theme_requests_.clear();
}
} // namespace prism::sdk
