#include "client_application_p.hpp"
#include "prism/runtime/owner_task_panel.hpp"
#include <cerrno>
#include <poll.h>
#include <system_error>

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
    return impl_->shaper.Ready() && impl_->commands.Ready();
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
    if (app.opened_once || app.closed || app.failed || !app.shaper.Ready() ||
        !app.commands.Ready() || app.config.app_id.empty()) {
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

    app.PublishFramePacket();
    app.DeliverInteractionEvents();
    return true;
}

bool ClientApplication::OpenPrepared(runtime::UiLoadId load,
                                     const runtime::PreparedComponent &prepared,
                                     runtime::LoadDiagnostic *diagnostic)
{
    auto &app = *impl_;
    if (app.opened_once || app.closed || app.failed || !app.shaper.Ready() ||
        !app.commands.Ready() || app.config.app_id.empty()) {
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
        app.CloseRenderWorker();
        app.ui_metrics = {};
        app.ui_configure_count = 0;
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
        if (!app.ResetRenderBridge() && diagnostic) {
            *diagnostic = {runtime::LoadStage::Install, prepared.Source(), 0,
                           "Render bridge recovery failed after window open"};
        }
        if (diagnostic) {
            if (diagnostic->message.empty()) {
                *diagnostic = {runtime::LoadStage::Install, prepared.Source(), 0,
                               "Wayland window open failed"};
            }
        }
        return false;
    }
    app.opened_once = true;
    return true;
}

void ClientApplication::Impl::FailFrontend()
{
    failed = true;
    RetireOwnerTasks(runtime::TaskCancelReason::FrontendFailed);
    RetireOwnerFeedback();
    if (scene) {
        scene->CancelInput();
        CollectGestureEvents();
        CollectControlEvents();
    }

    QueueStopRenderWorker();
    ui_load.Cancel();
    DiscardInstall(runtime::UiInstallState::Cancelled);
    ui_presentation.Clear();
    queued_frame.reset();
    ui_submitted_frame.reset();
    uploaded_image_versions.clear();
    pending_release_versions.clear();
    animation_worker_active = false;
    animation_deadline_ns.reset();
    pending_animation_finish_sequence.reset();
    ui_metrics = {};
    ui_configure_count = 0;
    on_ui_submitted = {};
    CloseRenderWorker();
    bridge->render_events.Close();
    bridge->render_commands.Close();
    while (bridge->render_commands.TryPop()) {
    }
    try {
        DeliverInteractionEvents();
    } catch (...) {
        control_delivery.Clear();
        pending_gestures.clear();
        delivered_gestures.clear();
    }
}

bool ClientApplication::Impl::PollResources()
{
    const auto updates = resources.Poll();
    for (const auto &update : updates) {
        if (!scene_images.contains(update.id.value) || !scene) {
            continue;
        }
        const auto *image = resources.Get(update.id);
        if (update.state != runtime::ImageState::Ready || !image || !RegisterImage(update.id) ||
            !scene->ImageReady(update.id, update.intrinsic_size)) {
            bridge->terminal.Fail(runtime::TerminalReason::ResourceFailure);
            FailFrontend();
            continue;
        }
        if (scene->PendingDirty() != runtime::Dirty::None) {
            InvalidateQueuedFrame();
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
    if (!app.scene || app.failed || app.closed) {
        return false;
    }

    try {
        if (app.bridge->terminal.Reason() != runtime::TerminalReason::None ||
            app.bridge->terminal.StopRequested()) {
            app.FailFrontend();
            return false;
        }

        // Resource completion and reverse protocol events are independent of
        // the Wayland poll; only the render worker waits on that display.
        if (app.PollResources()) {
            timeout_ms = 0;
        }
        app.ProcessRenderEvents(true);
        if (app.failed || app.bridge->terminal.Reason() != runtime::TerminalReason::None ||
            app.bridge->terminal.StopRequested()) {
            app.FailFrontend();
            return false;
        }
        app.DeliverInteractionEvents();
        if (app.closed || app.failed) {
            return false;
        }
        app.AdvanceAnimationDeadline();
        app.ReconcileOwnerFeedback();
        app.PublishFramePacket();
        if (app.ui_work_turn_started) {
            timeout_ms = 0;
        }

        std::vector<pollfd> sources;
        const int completion_fd = app.resources.CompletionFd();
        const std::size_t resource_sources = completion_fd >= 0 ? 1 : 0;
        sources.reserve(resource_sources + 2 + wake_fds.size());
        if (resource_sources) {
            sources.push_back({completion_fd, POLLIN, 0});
        }
        sources.push_back({app.bridge->render_events.Fd(), POLLIN, 0});
        sources.push_back({app.bridge->terminal.Fd(), POLLIN, 0});
        sources.insert(sources.end(), wake_fds.begin(), wake_fds.end());

        const int ready = poll(sources.data(), static_cast<nfds_t>(sources.size()),
                               app.FeedbackTimeoutMs(app.AnimationTimeoutMs(timeout_ms)));
        if (ready < 0 && errno != EINTR) {
            throw std::system_error(errno, std::generic_category(), "Client event poll");
        }
        for (std::size_t i = 0; i < wake_fds.size(); ++i) {
            wake_fds[i].revents = sources[resource_sources + 2 + i].revents;
        }

        if (app.bridge->terminal.Reason() != runtime::TerminalReason::None ||
            app.bridge->terminal.StopRequested()) {
            app.ProcessRenderEvents(false);
            app.FailFrontend();
            return false;
        }
        app.ProcessRenderEvents(true);
        if (app.failed || app.bridge->terminal.Reason() != runtime::TerminalReason::None ||
            app.bridge->terminal.StopRequested()) {
            app.FailFrontend();
            return false;
        }

        app.PollResources();
        if (app.failed || app.bridge->terminal.Reason() != runtime::TerminalReason::None ||
            app.bridge->terminal.StopRequested()) {
            app.FailFrontend();
            return false;
        }
        app.DeliverInteractionEvents();
        if (app.closed || app.failed) {
            return false;
        }
        app.AdvanceAnimationDeadline();
        app.ReconcileOwnerFeedback();
        app.PublishFramePacket();
        return true;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[prism-sdk] event pump failed: %s\n", error.what());
        app.bridge->terminal.Fail(runtime::TerminalReason::InternalFailure);
        app.FailFrontend();
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
    if (!app.scene || runtime::IsOwnerTaskReservedName(name) ||
        runtime::IsOwnerFeedbackReservedName(name) || !app.scene->AcceptsBinding(name, value)) {
        return false;
    }
    app.binding_values.insert_or_assign(std::string(name), value);
    const bool changed = app.scene->SetBinding(name, std::move(value));
    app.ReconcileOwnerTask();
    if (changed &&
        (app.scene->PendingDirty() != runtime::Dirty::None || app.scene->HasActiveAnimations())) {
        app.InvalidateQueuedFrame();
    }
    if (changed && (app.scene->HasActiveAnimations() || app.animation_worker_active)) {
        app.SyncAnimationSampling();
        if (app.scene->HasActiveAnimations()) {
            app.QueueRenderUpdate(true);
        }
    }
    app.DeliverInteractionEvents();
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
        if (app.scene) {
            app.ReconcileOwnerTask();
            app.UpdateOwnerConfirmationText();
            app.UpdateOwnerFilePanelText();
            app.UpdateOwnerFeedbackText();
            // A resolved no-op can still change the accepted theme identity
            // while an older visual candidate is waiting for submission.
            app.InvalidateQueuedFrame();
            app.PublishFramePacket();
            app.SyncAnimationSampling();
        }
        app.DeliverInteractionEvents();
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

void ClientApplication::OnCloseRequested(std::function<bool()> callback)
{
    impl_->on_close_requested = std::move(callback);
}

void ClientApplication::OnControlValue(std::function<void(const runtime::ControlEdit &)> callback)
{
    impl_->control_delivery.SetHandler(std::move(callback));
}

void ClientApplication::OnTextEdit(std::function<void(std::string_view, std::string_view)> callback)
{
    impl_->on_text_edit = std::move(callback);
}

void ClientApplication::OnAction(std::function<void(std::string_view)> callback)
{
    impl_->on_action = std::move(callback);
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
    if (!impl_ || impl_->closed) {
        return;
    }

    impl_->closed = true;
    impl_->RetireOwnerTasks(runtime::TaskCancelReason::OwnerClosed);
    impl_->RetireOwnerFeedback();
    if (impl_->scene) {
        impl_->scene->CancelInput();
        impl_->CollectGestureEvents();
        impl_->CollectControlEvents();
    }

    impl_->QueueStopRenderWorker();
    impl_->CloseRenderWorker();

    impl_->ui_load.Cancel();
    impl_->DiscardInstall(runtime::UiInstallState::Cancelled);
    impl_->ClearPreloadedImages();
    impl_->ui_presentation.Clear();
    impl_->queued_frame.reset();
    impl_->ui_submitted_frame.reset();
    impl_->uploaded_image_versions.clear();
    impl_->pending_release_versions.clear();
    impl_->animation_worker_active = false;
    impl_->animation_deadline_ns.reset();
    impl_->pending_animation_finish_sequence.reset();
    impl_->on_ui_submitted = {};

    if (impl_->scene) {
        AddSceneStats(impl_->render_stats, impl_->scene->GetRenderStats());
    }
    impl_->scene.reset();
    impl_->last_list.reset();
    impl_->last_image_uses.reset();
    const auto discarded_images = impl_->scene_images;
    impl_->scene_images.clear();
    for (auto value : discarded_images) {
        impl_->DropImage({value});
    }

    impl_->ui_metrics = {};
    impl_->ui_configure_count = 0;
    impl_->bridge->render_events.Close();
    impl_->bridge->render_commands.Close();
    while (impl_->bridge->render_events.TryPop()) {
    }
    while (impl_->bridge->render_commands.TryPop()) {
    }
    impl_->DeliverInteractionEvents();
}

} // namespace prism::sdk
