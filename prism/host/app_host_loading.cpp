#include "app_host_p.hpp"

namespace prism::sdk {
using namespace host_detail;

bool AppHost::Impl::StartMasterPreparation()
{
    if (ui.master_load.owner || failed || closed) {
        return !failed && !closed;
    }

    ui.master_load = frontend->BeginUiLoad();
    startup.master_queued_ns = MonotonicNs();
    const auto result = master_loader->Submit({ui.master_load, package->ui, package->root});
    if (result != runtime::LoadSubmitResult::Accepted) {
        return Fail(contracts::LaunchError::RuntimeFailed, "Cannot queue Master preparation");
    }
    return true;
}

void AppHost::Impl::UiSubmitted(runtime::UiLoadId load)
{
    if (load != ui.preview_load || !package || failed || closed) {
        return;
    }
    ui.preview_submitted = true;
    if (!startup.preview_submitted_ns) {
        startup.preview_submitted_ns = MonotonicNs();
    }

    // Only dispatch pure CPU work here. Installation never runs inside a
    // Wayland submission callback, and the completion FD is already polled.
    try {
        StartMasterPreparation();
    } catch (const std::exception &error) {
        Fail(contracts::LaunchError::RuntimeFailed, error.what());
    }
}

void AppHost::Impl::TakeMasterCompletion()
{
    while (auto completion = master_loader->TakeCompletion()) {
        if (completion->load != ui.master_load || failed || closed) {
            continue;
        }
        ui.read_us = completion->timings.read_us;
        ui.prepare_us = completion->timings.prepare_us;
        ui.master_prepared = completion->prepared.has_value();
        if (ui.master_prepared) {
            startup.master_prepared_ns = MonotonicNs();
        }
        master_completion = std::move(completion);
    }
}

bool AppHost::Impl::InstallMaster()
{
    if (!master_completion || (package->preview && !ui.preview_presented)) {
        return true;
    }
    const auto &completion = *master_completion;
    if (completion.diagnostic) {
        ui.master_diagnostic = completion.diagnostic;
        const auto code = completion.diagnostic->stage == runtime::LoadStage::Read
                              ? contracts::LaunchError::InvalidPackage
                              : contracts::LaunchError::RuntimeFailed;
        return Fail(code, UiFailureDetail(*completion.diagnostic));
    }
    if (!completion.prepared) {
        return Fail(contracts::LaunchError::RuntimeFailed, "Missing Master preparation result");
    }

    runtime::LoadDiagnostic diagnostic;
    if (!master_install_started) {
        ui.master_image_count = completion.prepared->Images().size();
        bindings.clear();
        if (completion.plan) {
            for (const auto &binding : completion.plan->bindings) {
                bindings.emplace(binding.name, binding.initial);
            }
        }
        if (!frontend->StartPreparedInstall(completion.load, *completion.prepared, &diagnostic)) {
            ui.master_diagnostic = diagnostic;
            return Fail(contracts::LaunchError::RuntimeFailed, UiFailureDetail(diagnostic));
        }
        master_install_started = true;
    }
    if (install_advanced) {
        return true;
    }
    install_advanced = true;
    const auto state = frontend->AdvanceUiInstall(bindings, &diagnostic);
    if (state == runtime::UiInstallState::Pending) {
        return true;
    }
    if (state != runtime::UiInstallState::Committed) {
        ui.master_diagnostic = diagnostic;
        return Fail(contracts::LaunchError::RuntimeFailed, UiFailureDetail(diagnostic));
    }

    ui.master_images_ready = true;
    ui.master_installed = true;
    startup.master_installed_ns = MonotonicNs();
    installed_plan = completion.plan;
    CollectBindings(completion.prepared->Root(), mounted_bindings);
    master_completion.reset();
    if (!window_open) {
        window_open = true;
        if (!frontend->HasPresentationFeedback()) {
            return Fail(contracts::LaunchError::PresentationFailed,
                        "Compositor lacks presentation-time");
        }
        Event(contracts::LaunchMilestone::RuntimeReady);
    }

    // Quick lifecycle entry stays on the owner. Modules submit preparation work
    // through the Host ABI and receive its result on a later owner turn.
    return StartBusiness();
}
} // namespace prism::sdk
