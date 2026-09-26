#include "prism/sdk/module_session.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace prism::sdk {
std::uint64_t MonotonicNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
namespace {
bool Valid(PrismStringViewV1 text, std::size_t limit) {
    return text.data && text.size > 0 && text.size <= limit &&
        std::string_view(text.data, text.size).find('\0') == std::string_view::npos;
}
}
ModuleSession::ModuleSession(const std::filesystem::path& module, std::string app_id,
    std::uint64_t instance, BindingSink bindings, LaunchSink launch, SubscribeSink subscribe)
    : module_(module), app_id_(std::move(app_id)), instance_id_(instance),
      bindings_(std::move(bindings)), launch_(std::move(launch)), subscribe_(std::move(subscribe)), host_{sizeof(host_), PRISM_APP_ABI_V1, this,
        SetBinding, Ready, Launch, Schedule, Subscribe} {}
ModuleSession::~ModuleSession() {
    tick_due_.reset();
    if (instance_) module_.Api().destroy(instance_);
    // AppModule unload follows destroy; modules must join their own threads there.
}
bool ModuleSession::Start() {
    if (started_ || !instance_id_ || !bindings_) return false;
    started_ = true;
    PrismAppInitV1 init{sizeof(init), PRISM_APP_ABI_V1, instance_id_,
        {app_id_.data(), app_id_.size()}, &host_};
    instance_ = module_.Api().create(&init);
    if (!instance_) { ready_ = false; tick_due_.reset(); }
    return instance_ != nullptr;
}
int32_t ModuleSession::SetBinding(void* ctx, PrismStringViewV1 key, PrismValueV1 value) noexcept {
    try {
        auto& self = *static_cast<ModuleSession*>(ctx);
        if (!Valid(key, 128)) return -1;
        runtime::PropertyValue converted;
        switch (value.kind) {
            case PRISM_VALUE_STRING_V1:
                if (value.as.string.size > 65536 || (!value.as.string.data && value.as.string.size)) return -1;
                converted = value.as.string.size ? std::string(value.as.string.data, value.as.string.size) : std::string{};
                if (std::get<std::string>(converted).find('\0') != std::string::npos) return -1;
                break;
            case PRISM_VALUE_NUMBER_V1:
                if (!std::isfinite(value.as.number)) return -1;
                converted = value.as.number;
                break;
            case PRISM_VALUE_BOOL_V1:
                if (value.as.boolean > 1) return -1;
                converted = value.as.boolean != 0;
                break;
            case PRISM_VALUE_COLOR_V1:
                converted = contracts::Color{static_cast<uint8_t>(value.as.rgba >> 24),
                    static_cast<uint8_t>(value.as.rgba >> 16),
                    static_cast<uint8_t>(value.as.rgba >> 8), static_cast<uint8_t>(value.as.rgba)};
                break;
            default: return -1;
        }
        return self.bindings_(std::string_view(key.data, key.size), std::move(converted)) ? 0 : -1;
    } catch (...) { return -1; }
}
int32_t ModuleSession::Ready(void* ctx) noexcept {
    auto& self = *static_cast<ModuleSession*>(ctx);
    if (self.ready_) return -1;
    self.ready_ = true;
    return 0;
}
uint64_t ModuleSession::Launch(void* context, PrismStringViewV1 app_id) noexcept {
    try {
        auto& self = *static_cast<ModuleSession*>(context);
        if (!Valid(app_id, 128) || !self.launch_ || self.launches_.size() >= 64) return 0;
        const std::string_view name(app_id.data, app_id.size);
        auto id = self.launch_(name);
        if (!id) return 0;
        self.launches_.emplace(id, PendingLaunch{std::string(name), {}, 0, false});
        return id;
    } catch (...) { return 0; }
}
uint64_t ModuleSession::Subscribe(void* ctx) noexcept {
    try {
        auto& self=*static_cast<ModuleSession*>(ctx);
        if (self.subscription_) return self.subscription_;
        if (!self.subscribe_) return 0;
        return self.subscription_=self.subscribe_();
    } catch (...) { return 0; }
}
void ModuleSession::Deliver(const contracts::InstanceUpdate& event) {
    if (!subscription_ || event.request.value!=subscription_ || !instance_ || !module_.Api().on_instance_event) return;
    PrismInstanceEventV1 projected{sizeof(projected),event.request.value,event.instance.value,event.pid,
        static_cast<std::uint32_t>(event.change),{event.app_id.data(),event.app_id.size()}};
    module_.Api().on_instance_event(instance_,&projected);
}
int32_t ModuleSession::Schedule(void* ctx, uint64_t delay) noexcept {
    auto& self = *static_cast<ModuleSession*>(ctx);
    const auto now = MonotonicNs();
    if (!self.module_.Api().on_tick || delay > std::numeric_limits<uint64_t>::max() - now) return -1;
    self.tick_due_ = now + delay;
    return 0;
}
void ModuleSession::Action(std::string_view action) {
    if (instance_ && module_.Api().on_action)
        module_.Api().on_action(instance_, {action.data(), action.size()});
}
void ModuleSession::Tick(std::uint64_t now) {
    if (!instance_ || !tick_due_ || now < *tick_due_) return;
    tick_due_.reset(); // A callback may schedule its next tick.
    module_.Api().on_tick(instance_, now);
}
int ModuleSession::TimeoutMs(std::uint64_t now, int maximum) const {
    maximum = std::max(0, maximum);
    if (!tick_due_) return maximum;
    if (now >= *tick_due_) return 0;
    return static_cast<int>(std::min<std::uint64_t>(maximum,
        (*tick_due_ - now) / 1000000 + ((*tick_due_ - now) % 1000000 != 0)));
}
} // namespace prism::sdk

namespace prism::sdk {
void ModuleSession::Deliver(const contracts::LaunchEvent& event) {
    auto it = launches_.find(event.request.value);
    if (it == launches_.end()) return;
    auto& pending = it->second;
    pending.instance = event.instance; pending.pid = event.pid;
    if (event.milestone == contracts::LaunchMilestone::Failed) pending.failed = true;
    if (instance_ && module_.Api().on_launch_event) {
        PrismLaunchEventV1 projected{sizeof(projected), event.request.value, event.instance.value,
            event.pid, static_cast<std::uint32_t>(event.milestone), static_cast<std::uint32_t>(event.error),
            event.exit_code, {pending.app_id.data(), pending.app_id.size()}, {event.detail.data(), event.detail.size()}};
        module_.Api().on_launch_event(instance_, &projected);
    }
    if (event.milestone == contracts::LaunchMilestone::Activated || event.milestone == contracts::LaunchMilestone::Exited ||
        (event.milestone == contracts::LaunchMilestone::Failed && !event.pid)) launches_.erase(it);
}
void ModuleSession::Disconnected() {
    if (subscription_) Deliver(contracts::InstanceUpdate{{subscription_},{},0,contracts::InstanceChange::Reset,{}});
    auto pending = launches_;
    for (const auto& [id, launch] : pending) if (!launch.failed)
        Deliver({{id}, launch.instance, launch.pid, contracts::LaunchMilestone::Failed,
            contracts::LaunchError::SessionEnded, 0, "Launch service disconnected"});
    launches_.clear();
}
} // namespace prism::sdk
