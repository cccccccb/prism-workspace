#include "client_application_p.hpp"

namespace prism::sdk {
void ClientApplication::Impl::CommitScene(runtime::UiLoadId load,
                                          std::unique_ptr<runtime::Scene> next,
                                          const std::set<std::uint64_t> &images)
{
    auto next_images = images;
    next->EnableAnimations();
    QueueRenderInstallUi(load);

    if (scene) {
        scene->CancelInput();
        CollectGestureEvents();
        CollectControlEvents();
        AddSceneStats(render_stats, scene->GetRenderStats());
    }
    ResetPopupSurface();
    ReleaseUnusedImages(images);
    preloaded_images.clear();
    preloaded_ui = {};
    scene = std::move(next);
    scene_images.swap(next_images);
    installed_ui = load;
    animation_worker_active = false;
    animation_deadline_ns.reset();
    pending_animation_finish_sequence.reset();
    ui_presentation.Install(load);
    last_list.reset();
    last_image_uses.reset();
    queued_frame.reset();
    ui_submitted_frame.reset();
    ui_root_metadata_frame.reset();
    force_frame_capture = true;
}

bool ClientApplication::Impl::OpenWindow(runtime::LoadDiagnostic *diagnostic,
                                         const runtime::ComponentSource &source)
{
    std::string failure;
    std::string detail;
    if (!OpenRenderWorker(&failure, &detail)) {
        if (diagnostic) {
            *diagnostic = {runtime::LoadStage::Install, source, 0,
                           failure.empty() ? "Wayland window open failed" : std::move(failure)};
        }
        return false;
    }
    opened_once = true;
    return true;
}

bool ClientApplication::Impl::CommitInstall(const runtime::BindingValues &bindings,
                                            runtime::LoadDiagnostic *diagnostic)
{
    auto &stage = *install;
    if (stage.constructed_theme != theme ||
        (stage.regions && stage.transaction_revision != scene->TransactionRevision())) {
        stage.ResetCandidate();
        ++install_stats.budget_yields;
        return true; // Rebase using the current live tree on the next owner turn.
    }

    std::string failure;
    const auto viewport = ui_configure_count
                              ? ui_metrics.logical_size
                              : contracts::LogicalSize{static_cast<double>(config.width),
                                                       static_cast<double>(config.height)};
    stage.scene->SetViewport(viewport);
    auto current = binding_values;
    for (const auto &[key, value] : bindings) {
        current.insert_or_assign(key, value);
    }

    if (stage.regions) {
        auto next_images = stage.result_images;
        const auto old_images = scene_images;
        if (!scene->MountRegions(stage.updates, current, *stage.scene, stage.transaction_revision,
                                 &failure)) {
            if (diagnostic) {
                *diagnostic = {runtime::LoadStage::Install, stage.units.front().prepared.Source(),
                               0, std::move(failure)};
            }
            return false;
        }

        // Retire the old candidate before releasing images it may still use.
        // The following frame is published after the new region tree is ready.
        QueueRenderInvalidate(installed_ui);
        queued_frame.reset();
        scene_images.swap(next_images);
        for (auto value : old_images) {
            if (!scene_images.contains(value) && !preloaded_images.contains(value)) {
                DropImage({value});
            }
        }
    } else {
        if (!stage.scene->PrepareDetached(current, &failure)) {
            if (diagnostic) {
                *diagnostic = {runtime::LoadStage::Install, stage.units.front().prepared.Source(),
                               0, std::move(failure)};
            }
            return false;
        }
        if (!opened_once && !OpenWindow(diagnostic, stage.units.front().prepared.Source())) {
            return false;
        }
        CommitScene(stage.load, std::move(stage.scene), std::move(stage.result_images));
    }
    queued_frame.reset();
    PublishFramePacket();
    SyncAnimationSampling();
    QueueRenderUpdate(true);
    binding_values.swap(current);
    install.reset();
    install_state = runtime::UiInstallState::Committed;
    if (diagnostic) {
        *diagnostic = {};
    }
    return true;
}
} // namespace prism::sdk
