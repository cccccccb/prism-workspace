#include "client_application_p.hpp"

namespace prism::sdk {
void ClientApplication::Impl::CommitScene(runtime::UiLoadId load,
                                          std::unique_ptr<runtime::Scene> next,
                                          const std::set<std::uint64_t> &images)
{
    auto next_images = images;
    std::deque<contracts::ResourceId> next_queue;
    std::set<std::uint64_t> next_queued;
    for (auto value : images) {
        if (registered_images.contains(value) && (!renderer || !renderer->ImageUploaded({value}))) {
            next_queue.push_back({value});
            next_queued.insert(value);
        }
    }
    if (scene) {
        AddSceneStats(render_stats, scene->GetRenderStats());
    }
    ReleaseUnusedImages(images);
    preloaded_images.clear();
    preloaded_ui = {};
    scene = std::move(next);
    scene_images.swap(next_images);
    upload_queue.swap(next_queue);
    queued_uploads.swap(next_queued);
    installed_ui = load;
    ui_presentation.Install(load);
    last_list.reset();
    committed_list.reset();
    prepared_list.reset();
    prepared_damage.reset();
    damage_history.Invalidate();
    committed_pixels_revision = prepared_pixels_revision = 0;
    state_prepared = false;
}

bool ClientApplication::Impl::OpenWindow(runtime::LoadDiagnostic *diagnostic,
                                         const runtime::ComponentSource &source)
{
    window.SetEventHandler(std::bind_front(&Impl::HandleWindowEvent, this));
    window.SetSubmitHandlers(std::bind_front(&Impl::PrepareSubmit, this),
                             std::bind_front(&Impl::CommitPixels, this),
                             std::bind_front(&Impl::Submitted, this));
    window.SetPresentationHandler(std::bind_front(&Impl::HandlePresentation, this));
    if (!window.Open(config.socket, config.app_id, config.title, config.width, config.height)) {
        window.Close();
        if (diagnostic) {
            *diagnostic = {runtime::LoadStage::Install, source, 0, "Wayland window open failed"};
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
    const auto viewport = window.IsConfigured()
                              ? window.Metrics().logical_size
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
    binding_values.swap(current);
    install.reset();
    install_state = runtime::UiInstallState::Committed;
    window.RequestUpdate(true);
    if (diagnostic) {
        *diagnostic = {};
    }
    return true;
}
} // namespace prism::sdk
