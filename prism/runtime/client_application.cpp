#include "client_application_p.hpp"

namespace prism::sdk {
std::optional<std::string> LoadUiSource(std::string_view installed_name,
                                        std::string_view source_path)
{
    const auto installed_ui =
        std::filesystem::canonical("/proc/self/exe").parent_path().parent_path() /
        "share/prism/ui" / installed_name;
    for (const auto &path :
         {std::string(source_path), "../" + std::string(source_path), installed_ui.string()}) {
        std::ifstream input(path);
        if (input) {
            return std::string(std::istreambuf_iterator<char>{input}, {});
        }
    }
    return std::nullopt;
}

ClientApplication::ClientApplication(ClientConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

ClientApplication::~ClientApplication()
{
    Close();
}

bool ClientApplication::FrontendReady() const
{
    return impl_->commands.Ready();
}

bool ClientApplication::ConfigureWindow(ClientConfig config)
{
    if (impl_->closed || impl_->opened_once || config.font_path != impl_->config.font_path) {
        return false;
    }
    if (config.task_scheduler != impl_->config.task_scheduler ||
        !config.install_limits.nodes_per_turn || !config.install_limits.images_per_turn ||
        !config.install_limits.upload_bytes_per_turn ||
        config.install_limits.cpu_per_turn.count() <= 0) {
        return false;
    }
    impl_->config = std::move(config);
    return true;
}

bool ClientApplication::ReplaceUi(std::string_view source)
{
    if (!impl_->opened_once || !impl_->scene || impl_->closed || impl_->failed) {
        return false;
    }

    try {
        const auto load = BeginUiLoad();
        return ReplaceUiPrepared(load, runtime::PrepareComponent(source));
    } catch (const std::exception &) {
        return false;
    }
}

bool ClientApplication::Open(std::string_view dsl_source)
{
    auto &app = *impl_;
    if (app.opened_once || app.closed || app.failed || !app.commands.Ready() ||
        app.config.app_id.empty()) {
        return false;
    }

    try {
        const auto load = BeginUiLoad();
        return OpenPrepared(load, runtime::PrepareComponent(dsl_source));
    } catch (const std::exception &) {
        return false;
    }
}

runtime::UiLoadId ClientApplication::BeginUiLoad()
{
    if (impl_->closed || impl_->failed) {
        return {};
    }
    impl_->DiscardInstall(runtime::UiInstallState::Cancelled);
    impl_->ClearPreloadedImages();
    return impl_->ui_load.Begin();
}

void ClientApplication::CancelUiLoad()
{
    impl_->ui_load.Cancel();
    impl_->DiscardInstall(runtime::UiInstallState::Cancelled);
    impl_->ClearPreloadedImages();
}

UiPresentationState ClientApplication::GetUiPresentation(runtime::UiLoadId load) const noexcept
{
    return impl_->ui_presentation.Get(load);
}

void ClientApplication::OnUiSubmitted(std::function<void(runtime::UiLoadId)> callback)
{
    impl_->on_ui_submitted = std::move(callback);
}

bool ClientApplication::ReplaceUiPrepared(runtime::UiLoadId load,
                                          const runtime::PreparedComponent &prepared,
                                          runtime::LoadDiagnostic *diagnostic)
{
    auto &app = *impl_;
    if (!app.opened_once || !app.scene) {
        if (diagnostic) {
            *diagnostic = {runtime::LoadStage::Install,
                           prepared ? prepared.Source() : runtime::ComponentSource{}, 0,
                           "UI replacement requires an open frontend"};
        }
        return false;
    }
    if (!app.InstallScene(load, prepared, diagnostic)) {
        return false;
    }

    app.window.RequestUpdate(true);
    return true;
}

bool ClientApplication::OpenPrepared(runtime::UiLoadId load,
                                     const runtime::PreparedComponent &prepared,
                                     runtime::LoadDiagnostic *diagnostic)
{
    auto &app = *impl_;
    if (app.opened_once || app.closed || app.failed || !app.commands.Ready() ||
        app.config.app_id.empty()) {
        if (diagnostic) {
            *diagnostic = {runtime::LoadStage::Install,
                           prepared ? prepared.Source() : runtime::ComponentSource{}, 0,
                           "Frontend cannot open a new window"};
        }
        return false;
    }
    const auto previous_ui = app.installed_ui;
    if (!app.InstallScene(load, prepared, diagnostic)) {
        return false;
    }

    if (!app.OpenWindow(diagnostic, prepared.Source())) {
        if (app.scene) {
            AddSceneStats(app.render_stats, app.scene->GetRenderStats());
        }
        app.window.Close();
        app.scene.reset();
        const auto discarded_images = app.scene_images;
        app.scene_images.clear();
        for (auto value : discarded_images) {
            if (!app.preloaded_images.contains(value)) {
                app.DropImage({value});
            }
        }
        app.installed_ui = previous_ui;
        app.ui_presentation.Clear();
        app.prepared_frame.reset();
        if (diagnostic) {
            *diagnostic = {runtime::LoadStage::Install, prepared.Source(), 0,
                           "Wayland window open failed"};
        }
        return false;
    }
    app.opened_once = true;
    return true;
}

void ClientApplication::Impl::CloseGpu()
{
    damage_history.Invalidate();
    prepared_damage.reset();
    prepared_frame.reset();
    committed_frame.reset();

    // EGL owns native objects backed by the Wayland surface. Release them
    // before the platform's terminal failure destroys that surface/display.
    // Another ClientApplication on this thread may have made its GL context
    // current. Ganesh must delete resources in this renderer's own context.
    if (renderer) {
        if (!egl.MakeCurrent()) {
            renderer->Abandon();
        }
        renderer.reset();
    }
    egl.Close();
}

void ClientApplication::Impl::FailFrontend()
{
    failed = true;
    ui_load.Cancel();
    DiscardInstall(runtime::UiInstallState::Cancelled);
    ui_presentation.Clear();
    prepared_frame.reset();
    on_ui_submitted = {};
    CloseGpu();
}

bool ClientApplication::Impl::PollResources()
{
    const auto updates = resources.Poll();
    for (const auto &update : updates) {
        if (!scene_images.contains(update.id.value)) {
            continue;
        }
        const auto *image = resources.Get(update.id);
        if (update.state != runtime::ImageState::Ready || !image || !RegisterImage(update.id) ||
            !scene->ImageReady(update.id, update.intrinsic_size)) {
            FailFrontend();
            continue;
        }
        QueueImageUpload(update.id);
        if (scene->PendingDirty() != runtime::Dirty::None) {
            window.RequestUpdate(true);
        }
    }
    return !updates.empty();
}

bool ClientApplication::Pump(int timeout_ms, std::span<pollfd> wake_fds)
{
    auto &app = *impl_;
    if (!app.ui_work_turn_explicit && !app.install_advanced_since_pump) {
        app.ResetUiWorkBudget();
    }
    Impl::PumpTurnGuard work_turn{app};
    for (auto &fd : wake_fds) {
        fd.revents = 0;
    }
    if (!app.scene || app.failed) {
        return false;
    }
    try {
        // A consumed resource completion must return control to the host even
        // if it belonged to a hidden subtree and produced no pixel dirtiness.
        if (app.PollResources()) {
            timeout_ms = 0;
        }
        if (app.failed) {
            app.window.Close();
            return false;
        }
        if (!app.AdvanceImageUploads()) {
            app.FailFrontend();
            app.window.Close();
            return false;
        }
        if (app.ui_work_turn_started || (!app.upload_queue.empty() && app.window.IsConfigured())) {
            timeout_ms = 0;
        }

        std::vector<pollfd> sources;
        const int completion_fd = app.resources.CompletionFd();
        const std::size_t resource_sources = completion_fd >= 0 ? 1 : 0;
        sources.reserve(resource_sources + wake_fds.size());
        if (resource_sources) {
            sources.push_back({completion_fd, POLLIN, 0});
        }
        sources.insert(sources.end(), wake_fds.begin(), wake_fds.end());

        const bool running = app.window.Pump(timeout_ms, sources);
        for (std::size_t i = 0; i < wake_fds.size(); ++i) {
            wake_fds[i].revents = sources[resource_sources + i].revents;
        }
        if (!running) {
            app.FailFrontend();
            app.window.Close();
            return false;
        }

        app.PollResources();
        if (app.failed) {
            app.window.Close();
            return false;
        }

        // Resource updates defer submission; the next host turn re-collects
        // its sources, then Window submits before entering the next wait.
        return true;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[prism-sdk] event pump failed: %s\n", error.what());
        app.FailFrontend();
        app.window.Close();
        return false;
    }
}

bool ClientApplication::SetSlot(std::string_view name, std::string value)
{
    return SetBinding(name, std::move(value));
}

bool ClientApplication::SetBinding(std::string_view name, runtime::PropertyValue value)
{
    auto &app = *impl_;
    if (!app.scene || !app.scene->AcceptsBinding(name, value)) {
        return false;
    }
    app.binding_values.insert_or_assign(std::string(name), value);
    app.scene->SetBinding(name, std::move(value));
    if (app.scene->PendingDirty() != runtime::Dirty::None) {
        app.window.RequestUpdate(true);
    }
    return true;
}

bool ClientApplication::ApplyTheme(const contracts::ThemeSnapshot &theme, std::string *diagnostic)
{
    auto &app = *impl_;
    try {
        contracts::ValidateTheme(theme);
        std::optional<contracts::ThemeSnapshot> prepared(theme);
        if (app.scene && !app.scene->ApplyTheme(theme, diagnostic)) {
            return false;
        }
        app.theme.swap(prepared);
        if (app.scene && app.scene->PendingDirty() != runtime::Dirty::None) {
            app.window.RequestUpdate(true);
        }
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

std::uint64_t ClientApplication::ThemeGeneration() const
{
    return impl_->theme ? impl_->theme->generation : 0;
}

void ClientApplication::OnAction(std::function<void(std::string_view)> callback)
{
    impl_->on_action = std::move(callback);
}

bool ClientApplication::IsCloseRequested() const
{
    return impl_->window.IsCloseRequested();
}

bool ClientApplication::IsMapped() const
{
    return impl_->window.IsMapped();
}

int ClientApplication::ConfigureCount() const
{
    return impl_->window.ConfigureCount();
}

int ClientApplication::FrameDoneCount() const
{
    return impl_->window.FrameDoneCount();
}

bool ClientApplication::FrameCallbackPending() const
{
    return impl_->window.FrameCallbackPending();
}

int ClientApplication::PresentedCount() const
{
    return impl_->presented;
}

bool ClientApplication::HasPresentationFeedback() const
{
    return impl_->window.HasPresentationFeedback();
}

int ClientApplication::PresentationCount() const
{
    return impl_->window.PresentationCount();
}

std::uint64_t ClientApplication::WaitDurationNs() const noexcept
{
    return impl_->window.WaitDurationNs();
}

ClientRenderStats ClientApplication::GetRenderStats() const
{
    auto stats = impl_->render_stats;
    if (impl_->scene) {
        AddSceneStats(stats, impl_->scene->GetRenderStats());
    }
    stats.frame_callbacks_done = static_cast<std::uint64_t>(impl_->window.FrameDoneCount());
    const auto submitted = impl_->window.GetSubmitStats();
    stats.surface_state_commits = submitted.state_commits;
    stats.surface_pixel_commits = submitted.pixel_commits;
    stats.surface_submission_failures = submitted.failures;
    stats.surface_noops = submitted.none;
    return stats;
}

ClientStartupStats ClientApplication::GetStartupStats() const noexcept
{
    return impl_->startup_stats;
}

int ClientApplication::RequestedImageCount() const
{
    return impl_->requested_images;
}

int ClientApplication::LoadedImageCount() const
{
    return impl_->loaded_images;
}

std::string ClientApplication::GlRenderer() const
{
    return impl_->gl_renderer;
}

void ClientApplication::Close()
{
    if (!impl_) {
        return;
    }
    impl_->closed = true;
    impl_->ui_load.Cancel();
    impl_->DiscardInstall(runtime::UiInstallState::Cancelled);
    impl_->ClearPreloadedImages();
    impl_->ui_presentation.Clear();
    impl_->prepared_frame.reset();
    impl_->on_ui_submitted = {};
    impl_->window.SetPresentationHandler({});

    impl_->CloseGpu();
    impl_->window.Close();
    if (impl_->scene) {
        AddSceneStats(impl_->render_stats, impl_->scene->GetRenderStats());
    }
    impl_->scene.reset();
    impl_->last_list.reset();
    const auto discarded_images = impl_->scene_images;
    impl_->scene_images.clear();
    for (auto value : discarded_images) {
        impl_->DropImage({value});
    }
    impl_->upload_queue.clear();
    impl_->queued_uploads.clear();
}
} // namespace prism::sdk
