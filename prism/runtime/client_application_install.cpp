#include "client_application_p.hpp"
#include <algorithm>
#include <chrono>

namespace prism::sdk {
namespace {
using Clock = std::chrono::steady_clock;

class InstallTurn {
public:
    explicit InstallTurn(runtime::UiInstallStats &stats) : stats_(stats)
    {
    }

    ~InstallTurn()
    {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start_);
        stats_.max_turn_us =
            std::max(stats_.max_turn_us, static_cast<std::uint64_t>(elapsed.count()));
    }

private:
    runtime::UiInstallStats &stats_;
    Clock::time_point start_{Clock::now()};
};

bool Reject(runtime::LoadDiagnostic *diagnostic, const runtime::ComponentSource &source,
            std::string message, runtime::LoadStage stage = runtime::LoadStage::Install)
{
    if (diagnostic) {
        *diagnostic = {stage, source, 0, std::move(message)};
    }
    return false;
}

void CollectImages(StagedUiInstall &stage)
{
    std::set<std::string> uris;
    for (const auto &unit : stage.units) {
        for (const auto &image : unit.prepared.Images()) {
            if (uris.insert(image.uri).second) {
                stage.images.push_back({image.uri});
            }
        }
    }
}

void CollectImageIds(const runtime::Blueprint &node, std::set<std::uint64_t> &images)
{
    for (const auto &property : node.properties) {
        if (property.id == runtime::DslProperty::Source) {
            if (const auto *id = std::get_if<contracts::ResourceId>(&property.value); id && *id) {
                images.insert(id->value);
            }
        }
    }
    for (const auto &child : node.children) {
        CollectImageIds(child, images);
    }
}
} // namespace

ClientApplication::Impl::~Impl() = default;

void ClientApplication::Impl::DiscardInstall(runtime::UiInstallState state)
{
    if (install) {
        for (auto value : install->owned_images) {
            if (!scene_images.contains(value) && !preloaded_images.contains(value)) {
                DropImage({value});
            }
        }
        install.reset();
    }
    install_state = state;
}

bool ClientApplication::StartPreparedInstall(runtime::UiLoadId load,
                                             const runtime::PreparedComponent &prepared,
                                             runtime::LoadDiagnostic *diagnostic)
{
    auto &app = *impl_;
    const auto source = prepared ? prepared.Source() : runtime::ComponentSource{};
    if (!prepared || !app.ui_load.Current(load) || app.failed || app.closed ||
        app.installed_ui == load || !app.commands.Ready() || app.config.app_id.empty()) {
        return Reject(diagnostic, source, "Cannot stage this critical UI load");
    }
    if (app.install) {
        return Reject(diagnostic, source, "A UI installation is already pending");
    }
    const auto &limits = app.config.install_limits;
    if (!limits.nodes_per_turn || !limits.images_per_turn || !limits.upload_bytes_per_turn ||
        limits.cpu_per_turn.count() <= 0) {
        return Reject(diagnostic, source, "UI installation limits must be positive");
    }

    auto next = std::make_unique<StagedUiInstall>();
    next->load = load;
    next->units.push_back({{}, prepared});
    CollectImages(*next);
    app.install = std::move(next);
    app.install_state = runtime::UiInstallState::Pending;
    if (diagnostic) {
        *diagnostic = {};
    }
    return true;
}

bool ClientApplication::StartRegionInstall(runtime::UiLoadId load,
                                           std::span<const runtime::PreparedRegion> regions,
                                           runtime::LoadDiagnostic *diagnostic)
{
    auto &app = *impl_;
    if (!app.scene || !app.opened_once || !app.ui_load.Current(load) || app.installed_ui != load ||
        app.failed || app.closed || app.install || regions.empty()) {
        return Reject(diagnostic, {}, "Cannot stage this region batch");
    }
    const auto &limits = app.config.install_limits;
    if (!limits.nodes_per_turn || !limits.images_per_turn || !limits.upload_bytes_per_turn ||
        limits.cpu_per_turn.count() <= 0) {
        return Reject(diagnostic, {}, "UI installation limits must be positive");
    }

    auto next = std::make_unique<StagedUiInstall>();
    next->load = load;
    next->regions = true;
    std::set<std::string> names;
    for (const auto &region : regions) {
        if (!region.prepared || region.region.empty() || !names.insert(region.region).second) {
            return Reject(diagnostic,
                          region.prepared ? region.prepared.Source() : runtime::ComponentSource{},
                          "Invalid or repeated prepared region");
        }
        next->units.push_back({region.region, region.prepared});
    }
    CollectImages(*next);
    app.install = std::move(next);
    app.install_state = runtime::UiInstallState::Pending;
    if (diagnostic) {
        *diagnostic = {};
    }
    return true;
}

bool ClientApplication::UiInstallPending() const noexcept
{
    return impl_->install != nullptr;
}

bool ClientApplication::UiInstallNeedsWork() const noexcept
{
    const auto &app = *impl_;
    if (!app.upload_queue.empty() && app.window.IsConfigured()) {
        return true;
    }
    if (!app.install) {
        return false;
    }
    const auto &stage = *app.install;
    if (stage.requested != stage.images.size()) {
        return true;
    }
    for (const auto &image : stage.images) {
        if (app.resources.State(image.id) == runtime::ImageState::Loading) {
            return false;
        }
    }
    return !stage.regions || app.window.IsConfigured();
}

runtime::UiInstallStats ClientApplication::GetUiInstallStats() const noexcept
{
    return impl_->install_stats;
}

runtime::UiInstallState ClientApplication::AdvanceUiInstall(const runtime::BindingValues &bindings,
                                                            runtime::LoadDiagnostic *diagnostic)
{
    auto &app = *impl_;
    if (diagnostic) {
        *diagnostic = {};
    }
    if (!app.install) {
        return app.install_state;
    }
    if (!app.ui_load.Current(app.install->load) || app.closed || app.failed) {
        Reject(diagnostic, app.install->units.front().prepared.Source(), "UI staging cancelled",
               runtime::LoadStage::Cancelled);
        app.DiscardInstall(runtime::UiInstallState::Cancelled);
        return app.install_state;
    }

    InstallTurn turn(app.install_stats);
    if (!app.ui_work_turn_explicit) {
        app.ResetUiWorkBudget();
    }
    app.install_advanced_since_pump = true;
    auto &stage = *app.install;
    try {
        app.PollResources();
        if (app.failed || !app.install) {
            Reject(diagnostic, {}, "Live resource failure cancelled UI staging",
                   runtime::LoadStage::Cancelled);
            return app.install_state;
        }
        if ((stage.construction || stage.scene) &&
            (stage.constructed_theme != app.theme ||
             (stage.regions && stage.transaction_revision != app.scene->TransactionRevision()))) {
            stage.ResetCandidate();
        }
        if (stage.requested < stage.images.size()) {
            app.EnsureUiWorkBudget();
        }
        while (stage.requested < stage.images.size() &&
               app.owner_turn_requests < app.config.install_limits.images_per_turn &&
               Clock::now() < app.owner_turn_deadline) {
            auto &image = stage.images[stage.requested++];
            image.id = app.RequestImage(stage.owned_images, image.uri);
            stage.image_ids.emplace(image.uri, image.id);
            ++app.owner_turn_requests;
        }
        if (stage.requested != stage.images.size()) {
            ++app.install_stats.budget_yields;
            return runtime::UiInstallState::Pending;
        }
        for (const auto &image : stage.images) {
            const auto state = app.resources.State(image.id);
            if (state == runtime::ImageState::Failed) {
                throw runtime::LoadFailure({runtime::LoadStage::ResourceLink,
                                            stage.units.front().prepared.Source(), 0,
                                            "Required staged image is unavailable: " + image.uri});
            }
            if (state == runtime::ImageState::Loading) {
                return runtime::UiInstallState::Pending;
            }
        }

        app.EnsureUiWorkBudget();
        const auto deadline = app.owner_turn_deadline;
        while (stage.registered < stage.images.size() &&
               app.owner_turn_registrations < app.config.install_limits.images_per_turn &&
               Clock::now() < deadline) {
            auto &image = stage.images[stage.registered++];
            if (!app.RegisterImage(image.id)) {
                throw std::runtime_error("Staged image registration failed");
            }
            image.registered = true;
        }
        if (stage.registered != stage.images.size()) {
            ++app.install_stats.budget_yields;
            return runtime::UiInstallState::Pending;
        }

        if (app.window.IsConfigured() && !stage.images.empty()) {
            if (!app.EnsureRenderer(static_cast<int>(app.window.Metrics().buffer_size.width),
                                    static_cast<int>(app.window.Metrics().buffer_size.height))) {
                throw std::runtime_error("Staged GPU renderer is unavailable");
            }
            std::uint64_t bytes = app.owner_turn_upload_bytes;
            std::size_t uploaded = app.owner_turn_uploads;
            for (auto &image : stage.images) {
                if (app.renderer->ImageUploaded(image.id)) {
                    image.uploaded = true;
                    continue;
                }
                const auto size = app.resources.Get(image.id)->rgba.size();
                if (uploaded >= app.config.install_limits.images_per_turn ||
                    Clock::now() >= deadline ||
                    (uploaded &&
                     size > app.config.install_limits.upload_bytes_per_turn -
                                std::min(bytes, app.config.install_limits.upload_bytes_per_turn))) {
                    ++app.install_stats.budget_yields;
                    return runtime::UiInstallState::Pending;
                }
                if (!app.renderer->UploadImage(image.id)) {
                    throw std::runtime_error("Staged image GPU upload failed");
                }
                image.uploaded = true;
                ++uploaded;
                bytes += size;
                app.owner_turn_uploads = uploaded;
                app.owner_turn_upload_bytes = bytes;
                ++app.install_stats.image_uploads;
                app.install_stats.upload_bytes += size;
                app.install_stats.oversized_uploads +=
                    size > app.config.install_limits.upload_bytes_per_turn;
            }
        }

        if (Clock::now() >= deadline) {
            ++app.install_stats.budget_yields;
            return runtime::UiInstallState::Pending;
        }
        if (!stage.construction && !stage.scene) {
            if (stage.updates.empty()) {
                for (const auto &unit : stage.units) {
                    stage.updates.push_back(
                        {unit.region,
                         runtime::LinkComponent(
                             unit.prepared, std::bind_front(&StagedUiInstall::Resolve, &stage))});
                }
            }
            auto blueprint = stage.regions ? app.scene->RegionBlueprint(stage.updates)
                                           : std::move(stage.updates.front().content);
            stage.result_images.clear();
            CollectImageIds(blueprint, stage.result_images);
            stage.transaction_revision = stage.regions ? app.scene->TransactionRevision() : 0;
            stage.constructed_theme = app.theme;
            stage.construction = std::make_unique<runtime::SceneConstruction>(
                std::move(blueprint), std::bind_front(&Impl::ShapeText, &app),
                app.commands.FontId(), app.theme);
        }
        if (stage.construction) {
            if (app.owner_turn_nodes >= app.config.install_limits.nodes_per_turn) {
                ++app.install_stats.budget_yields;
                return runtime::UiInstallState::Pending;
            }
            const auto before = stage.construction->ConstructedNodes();
            const bool ready = stage.construction->Advance(
                app.config.install_limits.nodes_per_turn - app.owner_turn_nodes, deadline);
            const auto constructed = stage.construction->ConstructedNodes() - before;
            app.install_stats.nodes += constructed;
            app.owner_turn_nodes += constructed;
            if (!ready) {
                ++app.install_stats.budget_yields;
                return runtime::UiInstallState::Pending;
            }
            stage.scene = stage.construction->TakeScene();
            stage.construction.reset();
            for (auto value : stage.result_images) {
                stage.candidate_images.push_back({value});
            }
        }
        while (stage.candidate_images_ready < stage.candidate_images.size() &&
               app.owner_turn_image_ready < app.config.install_limits.images_per_turn &&
               Clock::now() < deadline) {
            const auto id = stage.candidate_images[stage.candidate_images_ready++];
            if (const auto *image = app.resources.Get(id)) {
                stage.scene->ImageReady(
                    id, {static_cast<double>(image->width), static_cast<double>(image->height)});
            }
            ++app.owner_turn_image_ready;
        }
        if (stage.candidate_images_ready != stage.candidate_images.size() ||
            Clock::now() >= deadline) {
            ++app.install_stats.budget_yields;
            return runtime::UiInstallState::Pending;
        }
        if (!app.CommitInstall(bindings, diagnostic)) {
            app.DiscardInstall(runtime::UiInstallState::Failed);
        }
        return app.install_state;
    } catch (const runtime::LoadFailure &error) {
        if (diagnostic) {
            *diagnostic = error.Diagnostic();
        }
    } catch (const std::exception &error) {
        Reject(diagnostic, stage.units.front().prepared.Source(), error.what());
    }
    if (app.opened_once && !app.scene) {
        app.window.Close();
        app.opened_once = false;
    }
    app.DiscardInstall(runtime::UiInstallState::Failed);
    return app.install_state;
}
} // namespace prism::sdk
