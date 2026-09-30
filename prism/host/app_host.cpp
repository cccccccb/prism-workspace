#include "app_host_p.hpp"

namespace prism::sdk {
using namespace host_detail;

namespace {
class UiWorkTurn {
public:
    explicit UiWorkTurn(ClientApplication &frontend) : frontend_(frontend)
    {
        frontend_.BeginUiWorkTurn();
    }

    ~UiWorkTurn()
    {
        frontend_.EndUiWorkTurn();
    }

private:
    ClientApplication &frontend_;
};
} // namespace

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
    DurationTimer frontend_timer(self.startup.frontend_prepare_us);
    try {
        ClientConfig config;
        config.font_path = self.config.font_path;
        self.scheduler = std::make_shared<runtime::TaskScheduler>(self.config.task_budget,
                                                                  self.config.task_workers);
        config.task_scheduler = self.scheduler;
        config.install_limits = self.config.install_limits;
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

void AppHost::DeliverLayoutState(const contracts::LayoutStateEvent &event)
{
    if (impl_->business) {
        impl_->business->Deliver(event);
    }
}

void AppHost::DeliverLayoutControlResult(const contracts::LayoutControlResult &event)
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
    self.startup.bind_ns = MonotonicNs();
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
        config.install_limits = self.config.install_limits;
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
        const auto prepare_started = MonotonicNs();
        const auto prepared = PrepareUi(*package.preview, "preview");
        self.startup.preview_prepare_us = (MonotonicNs() - prepare_started) / 1000;
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
    self.install_advanced = false;
    self.business_work_dispatched = false;
    self.business_progress = false;
    for (auto &fd : wake_fds) {
        fd.revents = 0;
    }
    if (!self.package || self.failed || self.closed) {
        return false;
    }
    PumpProcessingTimer processing_timer(self.startup, *self.frontend, self.wait_duration_ns);
    try {
        UiWorkTurn work_turn(*self.frontend);
        self.frontend->PollImageResources();
        self.TakeMasterCompletion();
        self.TakeRegionCompletions();
        self.Observe();
        if (!wake_fds.empty()) {
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
        self.InstallRegions();

        self.DrainLaunches();
        self.DispatchBusinessWork();

        if (self.business) {
            self.business->Tick(MonotonicNs());
        }

        const auto now = MonotonicNs();
        int wait = self.business ? self.business->TimeoutMs(now, timeout) : timeout;
        if (self.business_progress || self.frontend->UiInstallNeedsWork() ||
            self.RegionsNeedWork()) {
            wait = 0;
        }
        if (!self.ui.master_presented || !self.ready) {
            wait = host::Timeout(now, self.startup_deadline, wait);
        }

        std::vector<pollfd> descriptors(wake_fds.begin(), wake_fds.end());
        for (auto &fd : descriptors) {
            fd.revents = 0;
        }
        descriptors.push_back({self.master_loader->Fd(), POLLIN, 0});
        if (self.business && self.business->WorkCompletionFd() >= 0) {
            descriptors.push_back({self.business->WorkCompletionFd(), POLLIN, 0});
        }
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
        } else {
            const auto wait_started = MonotonicNs();
            const int result = poll(descriptors.data(), descriptors.size(), wait);
            const int poll_error = errno;
            self.wait_duration_ns += MonotonicNs() - wait_started;
            if (result < 0 && poll_error != EINTR) {
                return self.Fail(contracts::LaunchError::RuntimeFailed,
                                 "Master/control completion wait failed");
            }
        }
        for (std::size_t i = 0; i < wake_fds.size(); ++i) {
            wake_fds[i].revents = descriptors[i].revents;
        }

        if (self.failed) {
            return false;
        }
        self.frontend->PollImageResources();
        self.TakeMasterCompletion();
        self.TakeRegionCompletions();
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
        self.InstallRegions();
        self.DispatchBusinessWork();

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
        self.frontend->OnGesture({});
    }
    if (self.business) {
        self.business->Disconnected();
        self.business->StopWork();
    }
    self.ui.business_work_pending = false;
    self.ui.business_work_completion_ready = false;
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
    self.pending_regions.clear();
    self.installing_regions.clear();
    self.installed_regions.clear();
    self.rejected_regions.clear();
    self.region_diagnostics.clear();
    self.scheduler.reset();
    self.package.reset();
}
} // namespace prism::sdk
