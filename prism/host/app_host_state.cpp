#include "app_host_p.hpp"

namespace prism::sdk {
using namespace host_detail;

void AppHost::Impl::Event(contracts::LaunchMilestone milestone, contracts::LaunchError error,
                          std::string detail)
{
    if (config.on_event) {
        config.on_event({config.request, config.instance, static_cast<std::uint32_t>(getpid()),
                         milestone, error, 0, std::move(detail)});
    }
}

bool AppHost::Impl::Fail(contracts::LaunchError error, std::string detail)
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

void AppHost::Impl::Observe()
{
    if (master_loader) {
        const auto stats = master_loader->Stats();
        ui.component_count = stats.component_count;
        ui.critical_prepared = stats.critical_prepared;
        ui.deferred_prepared = stats.deferred_prepared;
        ui.deferred_started = stats.deferred_started;
        ui.deferred_diagnostics =
            master_loader->DeferredDiagnostics().size() + region_diagnostics.size();
        ui.deferred_installed = installed_regions.size();
        ui.deferred_rejected = rejected_regions.size();
    }
    ui.install_stats = frontend->GetUiInstallStats();
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
} // namespace prism::sdk
