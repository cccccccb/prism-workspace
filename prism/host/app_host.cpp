#include "prism/sdk/app_host.hpp"
#include "prism/host/event_wait.hpp"
#include "prism/launch/error.hpp"
#include "prism/runtime/dsl_schema.hpp"
#include "prism/runtime/image_resources.hpp"
#include "prism/runtime/master_load_session.hpp"
#include "prism/sdk/client_application.hpp"
#include "prism/sdk/launch_client.hpp"
#include "prism/sdk/module_session.hpp"
#include "prism/theme/compiler.hpp"
#include <cerrno>
#include <cmath>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <unistd.h>
#include <vector>

namespace prism::sdk {
namespace {
std::string ReadUi(const std::filesystem::path &file)
{
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        throw launch::LaunchFailure(contracts::LaunchError::InvalidPackage,
                                    "Cannot read package UI");
    }
    std::string source;
    char bytes[4096];
    while (input.read(bytes, sizeof(bytes)) || input.gcount()) {
        source.append(bytes, input.gcount());
        if (source.size() > 1024 * 1024) {
            throw launch::LaunchFailure(contracts::LaunchError::InvalidPackage, "UI exceeds 1 MiB");
        }
    }
    if (input.bad() || source.empty()) {
        throw launch::LaunchFailure(contracts::LaunchError::InvalidPackage, "Invalid package UI");
    }
    return source;
}

runtime::PreparedComponent PrepareUi(const std::filesystem::path &file, std::string component)
{
    return runtime::PrepareComponent(ReadUi(file), {std::move(component), file.string(), {}});
}

std::string UiFailureDetail(const runtime::LoadDiagnostic &diagnostic)
{
    auto location = diagnostic.source.source_path.empty() ? diagnostic.source.component_id
                                                          : diagnostic.source.source_path;
    if (diagnostic.line > 0) {
        location += (location.empty() ? "DSL line " : ":") + std::to_string(diagnostic.line);
    }
    return location.empty() ? diagnostic.message : location + ": " + diagnostic.message;
}

bool CallerInputReady(std::span<const pollfd> descriptors)
{
    for (const auto &fd : descriptors) {
        if (fd.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
            return true;
        }
    }
    return false;
}

void CollectBindings(const runtime::PreparedNode &node, std::set<std::string, std::less<>> &names)
{
    for (const auto &binding : node.bindings) {
        names.insert(binding.name);
    }
    for (const auto &child : node.children) {
        CollectBindings(child, names);
    }
}

bool MatchesBinding(runtime::LoadBindingType type, const runtime::PropertyValue &value)
{
    switch (type) {
    case runtime::LoadBindingType::String:
        return std::holds_alternative<std::string>(value);
    case runtime::LoadBindingType::Number:
        return std::holds_alternative<double>(value) && std::isfinite(std::get<double>(value));
    case runtime::LoadBindingType::Boolean:
        return std::holds_alternative<bool>(value);
    case runtime::LoadBindingType::Color:
        return runtime::ValidPropertyValue(runtime::DslProperty::Foreground, value);
    }
    return false;
}
} // namespace

struct AppHost::Impl {
    explicit Impl(HostConfig value) : config(std::move(value))
    {
    }

    HostConfig config;
    std::unique_ptr<ClientApplication> frontend;
    std::unique_ptr<ModuleSession> business;
    std::unique_ptr<LaunchClient> launches;
    std::optional<launch::AppPackage> package;
    std::shared_ptr<runtime::TaskScheduler> scheduler;
    std::unique_ptr<runtime::MasterLoadSession> master_loader;
    std::optional<runtime::MasterLoadCompletion> master_completion;
    bool master_resources_requested{};
    std::shared_ptr<const runtime::LoadPlan> installed_plan;
    std::map<std::string, runtime::PropertyValue, std::less<>> bindings;
    std::set<std::string, std::less<>> mounted_bindings;
    HostUiState ui;
    std::uint64_t startup_deadline{};
    bool configured{}, presented{}, ready{}, failed{}, bound_once{}, launch_disconnected{};
    bool window_open{}, closed{};

    void Event(contracts::LaunchMilestone milestone,
               contracts::LaunchError error = contracts::LaunchError::None, std::string detail = {})
    {
        if (config.on_event) {
            config.on_event({config.request, config.instance, static_cast<std::uint32_t>(getpid()),
                             milestone, error, 0, std::move(detail)});
        }
    }

    bool Fail(contracts::LaunchError error, std::string detail)
    {
        if (!failed) {
            failed = true;
            ui.failed = true;
            if (frontend) {
                frontend->CancelUiLoad();
            }
            if (master_loader) {
                master_loader->Cancel(ui.master_load);
            }
            master_completion.reset();
            Event(contracts::LaunchMilestone::Failed, error, std::move(detail));
        }
        return false;
    }

    void Observe()
    {
        if (master_loader) {
            const auto stats = master_loader->Stats();
            ui.component_count = stats.component_count;
            ui.critical_prepared = stats.critical_prepared;
            ui.deferred_prepared = stats.deferred_prepared;
            ui.deferred_started = stats.deferred_started;
            ui.deferred_diagnostics = master_loader->DeferredDiagnostics().size();
        }
        if (ui.preview_load.owner) {
            const auto preview = frontend->GetUiPresentation(ui.preview_load);
            ui.preview_submitted |= preview.submitted;
            ui.preview_presented |= preview.presented;
        }
        if (ui.master_load.owner) {
            const auto master = frontend->GetUiPresentation(ui.master_load);
            ui.master_submitted |= master.submitted;
            ui.master_presented |= master.presented;
            if (ui.master_presented && master_loader) {
                master_loader->MasterPresented(ui.master_load);
            }
            ui.master_first_submission = master.first_submission;
            ui.master_presented_submission = master.last_presented_submission;
        }

        if (!configured && frontend->ConfigureCount()) {
            configured = true;
            Event(contracts::LaunchMilestone::SurfaceConfigured);
        }
        if (!presented && frontend->PresentationCount()) {
            presented = true;
            Event(contracts::LaunchMilestone::FirstPresented, contracts::LaunchError::None,
                  "GL renderer=" + frontend->GlRenderer());
        }
        if (!ready && business && business->BackendReady()) {
            ready = true;
            Event(contracts::LaunchMilestone::BackendReady);
        }
    }

    bool SetBinding(std::string_view key, runtime::PropertyValue value)
    {
        if (installed_plan && !installed_plan->legacy) {
            const auto declaration = std::find_if(
                installed_plan->bindings.begin(), installed_plan->bindings.end(),
                [&](const runtime::LoadBinding &binding) { return binding.name == key; });
            if (declaration == installed_plan->bindings.end() ||
                !MatchesBinding(declaration->type, value)) {
                return false;
            }
            if (mounted_bindings.contains(key) && !frontend->SetBinding(key, value)) {
                return false;
            }
            bindings.insert_or_assign(std::string(key), std::move(value));
            return true;
        }
        return frontend->SetBinding(key, std::move(value));
    }

    std::uint64_t LaunchApplication(std::string_view app_id)
    {
        if (config.launch_app) {
            return config.launch_app(app_id);
        }
        if (!launches) {
            launches = std::make_unique<LaunchClient>();
        }
        return launches->Launch(std::string(app_id));
    }

    std::uint64_t SubscribeInstances()
    {
        if (config.subscribe_instances) {
            return config.subscribe_instances();
        }
        if (!launches) {
            launches = std::make_unique<LaunchClient>();
        }
        return launches->SubscribeInstances();
    }

    std::uint64_t SelectTheme(std::string_view id)
    {
        if (config.select_theme) {
            return config.select_theme(id);
        }
        if (!launches) {
            launches = std::make_unique<LaunchClient>();
        }
        return launches->SelectTheme(std::string(id));
    }

    std::uint64_t SelectColorScheme(std::string_view scheme)
    {
        if (config.select_color_scheme) {
            return config.select_color_scheme(scheme);
        }
        if (!launches) {
            launches = std::make_unique<LaunchClient>();
        }
        return launches->SelectTheme({}, std::string(scheme));
    }

    void HandleAction(std::string_view action)
    {
        business->Action(action);
    }

    bool StartBusiness()
    {
        business = std::make_unique<ModuleSession>(package->module, package->manifest.app_id,
                                                   config.instance.value,
                                                   std::bind_front(&Impl::SetBinding, this),
                                                   std::bind_front(&Impl::LaunchApplication, this),
                                                   std::bind_front(&Impl::SubscribeInstances, this),
                                                   std::bind_front(&Impl::SelectTheme, this),
                                                   std::bind_front(&Impl::SelectColorScheme, this));

        if (!business->Start()) {
            return Fail(contracts::LaunchError::RuntimeFailed, "Business create failed");
        }

        frontend->OnAction(std::bind_front(&Impl::HandleAction, this));

        if (config.initial_theme) {
            const auto &theme = *config.initial_theme;
            business->Deliver(contracts::ThemeEvent{0,
                                                    theme.generation,
                                                    contracts::ThemeStatus::Current,
                                                    theme.id,
                                                    theme.name,
                                                    {},
                                                    theme.color_scheme});
        }
        return true;
    }

    bool StartMasterPreparation()
    {
        if (ui.master_load.owner || failed || closed) {
            return !failed && !closed;
        }

        ui.master_load = frontend->BeginUiLoad();
        const auto result = master_loader->Submit({ui.master_load, package->ui, package->root});
        if (result != runtime::LoadSubmitResult::Accepted) {
            return Fail(contracts::LaunchError::RuntimeFailed, "Cannot queue Master preparation");
        }
        return true;
    }

    void UiSubmitted(runtime::UiLoadId load)
    {
        if (load != ui.preview_load || !package || failed || closed) {
            return;
        }
        ui.preview_submitted = true;

        // Only dispatch pure CPU work here. Installation never runs inside a
        // Wayland submission callback, and the completion FD is already polled.
        try {
            StartMasterPreparation();
        } catch (const std::exception &error) {
            Fail(contracts::LaunchError::RuntimeFailed, error.what());
        }
    }

    void TakeMasterCompletion()
    {
        while (auto completion = master_loader->TakeCompletion()) {
            if (completion->load != ui.master_load || failed || closed) {
                continue;
            }
            ui.read_us = completion->timings.read_us;
            ui.prepare_us = completion->timings.prepare_us;
            ui.master_prepared = completion->prepared.has_value();
            master_completion = std::move(completion);
        }
    }

    bool InstallMaster()
    {
        if (!master_completion || (package->preview && !ui.preview_presented)) {
            return true;
        }
        if (master_completion->prepared && !master_resources_requested) {
            runtime::LoadDiagnostic diagnostic;
            if (!frontend->PreloadImages(ui.master_load, *master_completion->prepared,
                                         &diagnostic)) {
                ui.master_diagnostic = diagnostic;
                return Fail(contracts::LaunchError::RuntimeFailed, UiFailureDetail(diagnostic));
            }
            master_resources_requested = true;
            ui.master_image_count = master_completion->prepared->Images().size();
        }
        if (master_resources_requested) {
            const auto state = frontend->PreloadedImageState(ui.master_load);
            if (state == runtime::ImageState::Loading) {
                return true;
            }
            if (state == runtime::ImageState::Failed) {
                runtime::LoadDiagnostic diagnostic{runtime::LoadStage::ResourceLink,
                                                   master_completion->prepared->Source(), 0,
                                                   "Required Master image failed to load"};
                ui.master_diagnostic = diagnostic;
                return Fail(contracts::LaunchError::RuntimeFailed, UiFailureDetail(diagnostic));
            }
            ui.master_images_ready = true;
        }
        auto completion = std::move(*master_completion);
        master_completion.reset();
        if (completion.diagnostic) {
            const auto &diagnostic = *completion.diagnostic;
            ui.master_diagnostic = diagnostic;
            const auto code = diagnostic.stage == runtime::LoadStage::Read
                                  ? contracts::LaunchError::InvalidPackage
                                  : contracts::LaunchError::RuntimeFailed;
            return Fail(code, UiFailureDetail(diagnostic));
        }
        if (!completion.prepared) {
            return Fail(contracts::LaunchError::RuntimeFailed, "Missing Master preparation result");
        }

        runtime::LoadDiagnostic diagnostic;
        const bool installed =
            window_open
                ? frontend->ReplaceUiPrepared(completion.load, *completion.prepared, &diagnostic)
                : frontend->OpenPrepared(completion.load, *completion.prepared, &diagnostic);
        if (!installed) {
            ui.master_diagnostic = diagnostic;
            return Fail(contracts::LaunchError::RuntimeFailed, UiFailureDetail(diagnostic));
        }
        ui.master_installed = true;
        installed_plan = completion.plan;
        CollectBindings(completion.prepared->Root(), mounted_bindings);
        if (completion.plan) {
            for (const auto &binding : completion.plan->bindings) {
                if (!SetBinding(binding.name, binding.initial)) {
                    return Fail(contracts::LaunchError::RuntimeFailed,
                                "Cannot initialize declared binding: " + binding.name);
                }
            }
        }

        if (!window_open) {
            window_open = true;
            if (!frontend->HasPresentationFeedback()) {
                return Fail(contracts::LaunchError::PresentationFailed,
                            "Compositor lacks presentation-time");
            }
            Event(contracts::LaunchMilestone::RuntimeReady);
        }

        // dlopen/create still use the owner-thread ABI. Their nonblocking
        // preparation contract is a later step, separate from CPU DSL loading.
        return StartBusiness();
    }

    void DrainLaunches()
    {
        if (!launches || !business) {
            return;
        }
        for (const auto &event : launches->Pump(0)) {
            business->Deliver(event);
        }
        for (const auto &event : launches->TakeInstanceUpdates()) {
            business->Deliver(event);
        }
        for (const auto &event : launches->TakeThemeEvents()) {
            if (event.status != contracts::ThemeStatus::Rejected) {
                std::string diagnostic;
                if (!owner->ApplyTheme(theme::LoadTheme(theme::DefaultThemeRoot(), event.id,
                                                        event.generation, event.color_scheme),
                                       &diagnostic)) {
                    business->Deliver(contracts::ThemeEvent{
                        event.request, owner->ThemeGeneration(), contracts::ThemeStatus::Rejected,
                        event.id, event.name, std::move(diagnostic), event.color_scheme});
                    continue;
                }
            }
            business->Deliver(event);
        }
        if (launches->Connected()) {
            launch_disconnected = false;
        } else if (!launch_disconnected) {
            launch_disconnected = true;
            business->Disconnected();
        }
    }

    AppHost *owner{};
};

AppHost::AppHost(HostConfig config) : impl_(std::make_unique<Impl>(std::move(config)))
{
    impl_->owner = this;
}

AppHost::~AppHost()
{
    Close();
}

bool AppHost::PrepareFrontend()
{
    auto &self = *impl_;
    if (self.failed || self.bound_once || self.closed) {
        return false;
    }
    if (self.frontend) {
        return self.frontend->FrontendReady();
    }
    try {
        ClientConfig config;
        config.font_path = self.config.font_path;
        self.scheduler = std::make_shared<runtime::TaskScheduler>(self.config.task_budget);
        config.task_scheduler = self.scheduler;
        self.frontend = std::make_unique<ClientApplication>(std::move(config));
        if (!self.frontend->FrontendReady()) {
            return self.Fail(contracts::LaunchError::RuntimeFailed, "Font initialization failed");
        }
        if (self.config.initial_theme) {
            std::string diagnostic;
            if (!self.frontend->ApplyTheme(*self.config.initial_theme, &diagnostic)) {
                return self.Fail(contracts::LaunchError::RuntimeFailed, std::move(diagnostic));
            }
        }
        return true;
    } catch (const std::exception &error) {
        return self.Fail(contracts::LaunchError::RuntimeFailed, error.what());
    }
}

bool AppHost::Assign(contracts::RequestId request, contracts::InstanceId instance)
{
    if (impl_->bound_once || impl_->failed || impl_->closed || !request.value || !instance.value) {
        return false;
    }
    impl_->config.request = request;
    impl_->config.instance = instance;
    return true;
}

void AppHost::DeliverLaunchEvent(const contracts::LaunchEvent &event)
{
    if (impl_->business) {
        impl_->business->Deliver(event);
    }
}

void AppHost::DeliverInstanceEvent(const contracts::InstanceUpdate &event)
{
    if (impl_->business) {
        impl_->business->Deliver(event);
    }
}

void AppHost::DeliverThemeEvent(const contracts::ThemeEvent &event)
{
    if (impl_->business) {
        impl_->business->Deliver(event);
    }
}

bool AppHost::ApplyTheme(const contracts::ThemeSnapshot &theme, std::string *diagnostic)
{
    try {
        contracts::ValidateTheme(theme);
        std::optional<contracts::ThemeSnapshot> prepared(theme);
        if (impl_->frontend && !impl_->frontend->ApplyTheme(theme, diagnostic)) {
            return false;
        }
        impl_->config.initial_theme.swap(prepared);
        if (diagnostic) {
            diagnostic->clear();
        }
        return true;
    } catch (const std::exception &error) {
        if (diagnostic) {
            *diagnostic = error.what();
        }
        return false;
    }
}

std::uint64_t AppHost::ThemeGeneration() const
{
    return impl_->config.initial_theme ? impl_->config.initial_theme->generation : 0;
}

bool AppHost::Bind(const launch::AppPackage &package)
{
    auto &self = *impl_;
    if (self.bound_once || self.failed || self.closed || !self.config.request.value ||
        !self.config.instance.value) {
        return false;
    }
    if (!PrepareFrontend()) {
        return false;
    }

    self.bound_once = true;
    self.package = package;
    self.startup_deadline = MonotonicNs() + 10000000000ULL;
    try {
        if (!self.config.initial_theme) {
            const auto initial = theme::LoadTheme(theme::DefaultThemeRoot(), "glass", 1);
            std::string diagnostic;
            if (!ApplyTheme(initial, &diagnostic)) {
                return self.Fail(contracts::LaunchError::RuntimeFailed, std::move(diagnostic));
            }
        }

        const auto &manifest = package.manifest;
        ClientConfig config{self.config.socket,      manifest.app_id,
                            manifest.name,           self.config.font_path,
                            manifest.width,          manifest.height,
                            package.assets.string(), self.config.gpu_resource_cache_bytes};
        config.task_scheduler = self.scheduler;
        if (!self.frontend->ConfigureWindow(std::move(config))) {
            return self.Fail(contracts::LaunchError::RuntimeFailed,
                             "Frontend window configuration failed");
        }
        self.master_loader = std::make_unique<runtime::MasterLoadSession>(
            self.scheduler, self.config.prepare_component);
        self.frontend->OnUiSubmitted(std::bind_front(&Impl::UiSubmitted, &self));

        // Master-only packages use the same completion/installation pipeline.
        // Bind does not read or parse their UI, and Pump can wait on control FDs
        // before a Wayland window has been created.
        if (!package.preview) {
            return self.StartMasterPreparation();
        }

        self.ui.preview_load = self.frontend->BeginUiLoad();
        const auto prepared = PrepareUi(*package.preview, "preview");
        runtime::LoadDiagnostic diagnostic;
        if (!self.frontend->OpenPrepared(self.ui.preview_load, prepared, &diagnostic)) {
            return self.Fail(contracts::LaunchError::RuntimeFailed, UiFailureDetail(diagnostic));
        }
        self.window_open = true;
        if (!self.frontend->HasPresentationFeedback()) {
            return self.Fail(contracts::LaunchError::PresentationFailed,
                             "Compositor lacks presentation-time");
        }

        self.Event(contracts::LaunchMilestone::RuntimeReady);

        // The first Preview pixel submission dispatches pure Master work.
        // Replacement and business creation wait for this Preview's feedback.
        self.Observe();
        return true;
    } catch (const runtime::LoadFailure &error) {
        return self.Fail(contracts::LaunchError::RuntimeFailed,
                         UiFailureDetail(error.Diagnostic()));
    } catch (const launch::LaunchFailure &error) {
        return self.Fail(error.Code(), error.what());
    } catch (const std::exception &error) {
        return self.Fail(contracts::LaunchError::RuntimeFailed, error.what());
    }
}

bool AppHost::Pump(int timeout, std::span<pollfd> wake_fds)
{
    auto &self = *impl_;
    for (auto &fd : wake_fds) {
        fd.revents = 0;
    }
    if (!self.package || self.failed || self.closed) {
        return false;
    }
    try {

        self.frontend->PollImageResources();
        self.TakeMasterCompletion();
        self.Observe();
        if (self.master_completion && !wake_fds.empty()) {
            if (poll(wake_fds.data(), wake_fds.size(), 0) < 0 && errno != EINTR) {
                return self.Fail(contracts::LaunchError::RuntimeFailed,
                                 "Caller control readiness check failed");
            }
            if (CallerInputReady(wake_fds)) {
                return true;
            }
        }
        if (!self.InstallMaster()) {
            return false;
        }

        self.DrainLaunches();

        if (self.business) {
            self.business->Tick(MonotonicNs());
        }

        const auto now = MonotonicNs();
        int wait = self.business ? self.business->TimeoutMs(now, timeout) : timeout;
        if (!self.ui.master_presented || !self.ready) {
            wait = host::Timeout(now, self.startup_deadline, wait);
        }

        std::vector<pollfd> descriptors(wake_fds.begin(), wake_fds.end());
        for (auto &fd : descriptors) {
            fd.revents = 0;
        }
        descriptors.push_back({self.master_loader->Fd(), POLLIN, 0});
        if (!self.window_open) {
            descriptors.push_back({self.frontend->ResourceCompletionFd(), POLLIN, 0});
        }
        if (self.launches && self.launches->Connected()) {
            descriptors.push_back(
                {self.launches->Fd(),
                 static_cast<short>(POLLIN | (self.launches->WantsWrite() ? POLLOUT : 0)), 0});
            if (self.launches->HasCompleteFrame()) {
                wait = 0;
            }
        }

        if (self.window_open) {
            if (!self.frontend->Pump(wait, descriptors)) {
                if (self.frontend->IsCloseRequested()) {
                    return false;
                }
                return self.Fail(contracts::LaunchError::RuntimeFailed,
                                 "Wayland/render connection failed");
            }
        } else if (poll(descriptors.data(), descriptors.size(), wait) < 0 && errno != EINTR) {
            return self.Fail(contracts::LaunchError::RuntimeFailed,
                             "Master/control completion wait failed");
        }
        for (std::size_t i = 0; i < wake_fds.size(); ++i) {
            wake_fds[i].revents = descriptors[i].revents;
        }

        if (self.failed) {
            return false;
        }
        self.frontend->PollImageResources();
        self.TakeMasterCompletion();
        self.Observe();
        // The caller owns these descriptors and consumes control/theme/stop
        // messages only after Pump returns. Never install a completed Master
        // ahead of those messages, and never drain a caller FD here.
        if (CallerInputReady(wake_fds)) {
            return true;
        }
        self.DrainLaunches();
        if (!self.InstallMaster()) {
            return false;
        }

        if (self.business) {
            self.business->Tick(MonotonicNs());
        }
        self.Observe();

        if ((!self.ui.master_presented || !self.ready) && MonotonicNs() >= self.startup_deadline) {
            return self.Fail(contracts::LaunchError::Timeout,
                             "Master presentation/backend startup timeout");
        }
        return true;
    } catch (const runtime::LoadFailure &error) {
        return self.Fail(contracts::LaunchError::RuntimeFailed,
                         UiFailureDetail(error.Diagnostic()));
    } catch (const launch::LaunchFailure &error) {
        return self.Fail(error.Code(), error.what());
    } catch (const std::exception &error) {
        return self.Fail(contracts::LaunchError::RuntimeFailed, error.what());
    }
}

bool AppHost::IsCloseRequested() const
{
    return impl_->frontend && impl_->frontend->IsCloseRequested();
}

HostUiState AppHost::GetUiState() const
{
    return impl_->ui;
}

void AppHost::Close()
{
    auto &self = *impl_;
    if (self.closed) {
        return;
    }
    self.closed = true;
    self.ui.cancelled = self.ui.master_load.owner && !self.ui.master_presented && !self.failed;

    if (self.frontend) {
        self.frontend->CancelUiLoad();
        self.frontend->OnUiSubmitted({});
        self.frontend->OnAction({});
    }
    if (self.master_loader) {
        self.master_loader->Stop();
    }
    self.master_completion.reset();
    self.master_loader.reset();
    self.business.reset();
    self.launches.reset();
    self.frontend.reset();
    self.installed_plan.reset();
    self.bindings.clear();
    self.mounted_bindings.clear();
    self.scheduler.reset();
    self.package.reset();
}
} // namespace prism::sdk
