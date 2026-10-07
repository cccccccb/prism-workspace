#include "client_render_owner_p.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>

namespace prism::sdk {

bool ClientRenderOwner::DrainCommands()
{
    while (auto command = commands_.TryPop()) {
        if (std::holds_alternative<runtime::AcceptCloseCommand>(*command)) {
            window_.AcceptCloseRequest();
            PublishStatus();
            return true;
        }
        if (auto *installed = std::get_if<runtime::InstallUiCommand>(&*command)) {
            if (!installed->ui.owner || !installed->ui.generation) {
                throw std::runtime_error("Invalid render UI installation");
            }
            if (installed_ui_ != installed->ui) {
                ClosePopup();
                blocked_popup_request_.reset();
                installed_ui_ = installed->ui;
                root_target_.ResetSubmission();
                root_target_.input_ui = installed_ui_;
                animation_sampling_active_ = false;
                ResetFrameOpportunity();
            }
            continue;
        }

        if (auto *invalidated = std::get_if<runtime::InvalidateFrameCommand>(&*command)) {
            if (!invalidated->ui.owner || !invalidated->ui.generation) {
                throw std::runtime_error("Invalid render frame invalidation");
            }
            if (invalidated->ui == installed_ui_) {
                // A later UI mutation superseded the candidate already read
                // from the queue. Keep the last successful pixel baseline.
                root_target_.render_frame.reset();
                popup_frame_.reset();
                ApproveFrame({});
                if (!frame_opportunity_) {
                    spontaneous_animation_frame_allowed_ = false;
                }
                window_.RequestUpdate(true);
            }
            continue;
        }

        if (auto *frame = std::get_if<runtime::FrameCommand>(&*command)) {
            if (!frame->frame || frame->frame->ui != installed_ui_) {
                throw std::runtime_error("Render frame belongs to an uninstalled UI");
            }
            root_target_.render_frame = std::move(frame->frame);
            if (animation_sampling_active_) {
                ApproveFrame(!frame_opportunity_ && spontaneous_animation_frame_allowed_
                                 ? root_target_.render_frame
                                 : nullptr);
            }
            window_.RequestUpdate(true);
            continue;
        }

        if (auto *sampling = std::get_if<runtime::SetAnimationSamplingCommand>(&*command)) {
            SetAnimationSampling(*sampling);
            continue;
        }

        if (const auto *rejected = std::get_if<runtime::RejectPopupCommand>(&*command)) {
            if (rejected->ui == popup_ui_ && rejected->identity == PopupIdentity(popup_.Target())) {
                ClosePopup(true);
            }
            continue;
        }

        if (auto *answer = std::get_if<runtime::AnswerFrameOpportunityCommand>(&*command)) {
            AnswerFrameOpportunity(std::move(*answer));
            continue;
        }

        if (auto *request = std::get_if<runtime::RequestRenderCommand>(&*command)) {
            if (animation_sampling_active_ && !frame_opportunity_ && !approved_frame_) {
                spontaneous_animation_frame_allowed_ = false;
            }
            if (request->kind == runtime::RenderRequestKind::Redraw) {
                window_.RequestRedraw(request->deferred);
            } else {
                window_.RequestUpdate(request->deferred);
            }
            continue;
        }

        if (auto *processed = std::get_if<runtime::UiEventsProcessedCommand>(&*command)) {
            if (!processed->sequence || processed->sequence < processed_event_sequence_ ||
                processed->sequence > issued_event_sequence_) {
                throw std::runtime_error("Invalid processed window event sequence");
            }
            processed_event_sequence_ = processed->sequence;
            window_.RequestUpdate(true);
            continue;
        }

        if (auto *image = std::get_if<runtime::RegisterImageCommand>(&*command)) {
            if (!RegisterImage(std::move(*image))) {
                return false;
            }
            continue;
        }

        if (!ReleaseImage(std::get<runtime::ReleaseImageCommand>(*command))) {
            return false;
        }
    }
    return true;
}

void ClientRenderOwner::ResetFrameOpportunity() noexcept
{
    frame_opportunity_.reset();
    ApproveFrame({});
    opportunity_candidate_sequence_ = 0;
    spontaneous_animation_frame_allowed_ = false;
}

void ClientRenderOwner::ApproveFrame(std::shared_ptr<const runtime::FramePacket> frame)
{
    if (frame && frame == approved_frame_) {
        return;
    }
    approved_frame_ = std::move(frame);
    approved_root_consumed_ = false;
    approved_popup_consumed_ = true;
    if (!approved_frame_) {
        return;
    }

    const auto &child = approved_frame_->popup_surface_frame;
    const auto target = popup_.Target();
    approved_popup_consumed_ = !child || !target || child->ui != approved_frame_->ui ||
                               child->worker != worker_generation_ ||
                               child->identity != PopupIdentity(target);
}

void ClientRenderOwner::ConsumeApprovedRoot(
    const std::shared_ptr<const runtime::FramePacket> &frame) noexcept
{
    if (!animation_sampling_active_ || !approved_frame_ || frame != approved_frame_) {
        return;
    }
    approved_root_consumed_ = true;
    FinishApprovedFrame();
}

void ClientRenderOwner::ConsumeApprovedPopup(
    const std::shared_ptr<const runtime::PopupFramePacket> &frame) noexcept
{
    if (!animation_sampling_active_ || !approved_frame_ ||
        frame != approved_frame_->popup_surface_frame || !frame ||
        frame->identity != PopupIdentity(popup_.Target())) {
        return;
    }
    approved_popup_consumed_ = true;
    FinishApprovedFrame();
}

void ClientRenderOwner::FinishApprovedFrame() noexcept
{
    if (!approved_frame_ || !approved_root_consumed_ || !approved_popup_consumed_) {
        return;
    }
    ApproveFrame({});
    spontaneous_animation_frame_allowed_ = false;
    window_.RequestUpdate(true);
}

void ClientRenderOwner::SetAnimationSampling(runtime::SetAnimationSamplingCommand command)
{
    if (!command.ui.owner || !command.ui.generation) {
        throw std::runtime_error("Invalid animation sampling UI");
    }
    if (command.ui != installed_ui_ || command.active == animation_sampling_active_) {
        return;
    }

    animation_sampling_active_ = command.active;
    ResetFrameOpportunity();
    if (command.active) {
        // The packet already consumed by the worker is only a candidate. The
        // successful submission remains the damage and presentation baseline.
        root_target_.render_frame.reset();
    }
    window_.RequestUpdate(true);
}

void ClientRenderOwner::AnswerFrameOpportunity(runtime::AnswerFrameOpportunityCommand command)
{
    if (!frame_opportunity_ || command.ui != frame_opportunity_->ui ||
        command.worker != frame_opportunity_->worker ||
        command.configure_count != frame_opportunity_->configure_count ||
        command.id != frame_opportunity_->id ||
        command.configure_count != window_.ConfigureCount()) {
        return;
    }
    if (command.frame &&
        (command.frame->ui != command.ui ||
         command.frame->configure_count != command.configure_count || !command.frame->sequence)) {
        throw std::runtime_error("Invalid frame opportunity response packet");
    }
    if (command.frame && root_target_.committed_frame &&
        command.frame->ui == root_target_.committed_frame->ui &&
        command.frame->sequence < root_target_.committed_frame->sequence) {
        ResetFrameOpportunity();
        window_.RequestUpdate(true);
        return;
    }
    if (root_target_.render_frame &&
        root_target_.render_frame->sequence > opportunity_candidate_sequence_ &&
        (!command.frame || root_target_.render_frame->sequence > command.frame->sequence)) {
        // The UI published a newer packet after this opportunity was sent.
        // A stale response must not roll it back or park its only permit.
        ResetFrameOpportunity();
        window_.RequestUpdate(true);
        return;
    }

    frame_opportunity_.reset();
    opportunity_candidate_sequence_ = 0;
    spontaneous_animation_frame_allowed_ = true;
    ApproveFrame(std::move(command.frame));
    if (approved_frame_) {
        root_target_.render_frame = approved_frame_;
        window_.RequestUpdate(true);
    }
}

bool ClientRenderOwner::IssueFrameOpportunity()
{
    if (!animation_sampling_active_ || !installed_ui_.owner || !installed_ui_.generation ||
        frame_opportunity_ ||
        next_frame_opportunity_id_ == std::numeric_limits<std::uint64_t>::max()) {
        terminal_.Fail(runtime::TerminalReason::InternalFailure);
        return false;
    }

    const runtime::FrameOpportunityEvent opportunity{
        installed_ui_, worker_generation_, window_.ConfigureCount(), ++next_frame_opportunity_id_};
    if (!QueueEvent(runtime::RenderEvent(opportunity))) {
        return false;
    }

    frame_opportunity_ = opportunity;
    opportunity_candidate_sequence_ =
        root_target_.render_frame ? root_target_.render_frame->sequence : 0;
    return true;
}

bool ClientRenderOwner::RegisterImage(runtime::RegisterImageCommand command)
{
    if (!command.version.id || !command.version.generation || !command.pixels) {
        throw std::runtime_error("Invalid render image registration");
    }
    auto found = render_images_.find(command.version.id.value);
    if (found != render_images_.end() &&
        found->second.version.generation >= command.version.generation) {
        throw std::runtime_error("Out-of-order render image registration");
    }

    if (!damage_commands_->RegisterImage(command.version.id, command.pixels)) {
        throw std::runtime_error("Render damage image registration failed");
    }
    if (renderer_) {
        if (!root_target_.egl.MakeCurrent()) {
            throw std::runtime_error("Render image registration context unavailable");
        }
        if (found != render_images_.end()) {
            renderer_->UnregisterImage(command.version.id);
        }
        if (!renderer_->RegisterImage(command.version.id, command.pixels)) {
            throw std::runtime_error("GPU image registration failed");
        }
    }

    render_images_.insert_or_assign(command.version.id.value,
                                    RenderImage{command.version, std::move(command.pixels)});
    QueueImageUpload(command.version.id);
    return true;
}

bool ClientRenderOwner::ReleaseImage(runtime::ReleaseImageCommand command)
{
    if (!command.version.id || !command.version.generation) {
        throw std::runtime_error("Invalid render image release");
    }

    RetirePopupImage(command.version);
    auto found = render_images_.find(command.version.id.value);
    if (found != render_images_.end() && found->second.version == command.version) {
        if (renderer_ && !root_target_.egl.MakeCurrent()) {
            renderer_->Abandon();
            renderer_.reset();
            throw std::runtime_error("Render image release context unavailable");
        }
        if (renderer_) {
            renderer_->UnregisterImage(command.version.id);
        }
        damage_commands_->UnregisterImage(command.version.id);
        render_images_.erase(found);
        queued_uploads_.erase(command.version.id.value);
        std::erase(upload_queue_, command.version.id);
    }

    return QueueEvent(runtime::RenderEvent(runtime::ImageReleasedEvent{command.version}));
}

void ClientRenderOwner::QueueImageUpload(contracts::ResourceId id)
{
    if (queued_uploads_.insert(id.value).second) {
        upload_queue_.push_back(id);
    }
}

bool ClientRenderOwner::EnsureRenderer(int width, int height)
{
    if (!window_.IsConfigured() || width <= 0 || height <= 0) {
        return false;
    }
    if (!root_target_.egl.Ready()) {
        const auto started = std::chrono::steady_clock::now();
        const bool opened = (egl_context_.Ready() || egl_context_.Open(window_.Display())) &&
                            root_target_.egl.Open(egl_context_, window_.Surface(), width, height);
        if (!egl_init_sampled_) {
            startup_stats_.egl_init_us =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::steady_clock::now() - started)
                                               .count());
            egl_init_sampled_ = true;
        }
        if (!opened) {
            return false;
        }

        gl_renderer_ = root_target_.egl.GlRenderer();
        const auto capability = root_target_.egl.Capabilities();
        root_target_.backend_stats.buffer_age_supported = capability.buffer_age;
        root_target_.backend_stats.swap_damage_supported = capability.swap_damage;
        root_target_.backend_stats.partial_update_supported = capability.partial_update;
        root_target_.damage_history.Reset(
            {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)});
    }
    if (!root_target_.egl.Resize(width, height) || !root_target_.egl.MakeCurrent()) {
        return false;
    }
    if (!renderer_) {
        render_skia::GlesRendererOptions options;
        if (config_.gpu_resource_cache_bytes) {
            options.resource_cache_bytes = *config_.gpu_resource_cache_bytes;
        }

        const auto started = std::chrono::steady_clock::now();
        auto next = std::make_unique<render_skia::GlesRenderer>(
            config_.font_path, root_target_.egl.TargetIdentity(), options);
        if (!ganesh_init_sampled_) {
            startup_stats_.ganesh_init_us =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::steady_clock::now() - started)
                                               .count());
            ganesh_init_sampled_ = true;
        }
        if (!next->Ready()) {
            return false;
        }
        for (const auto &[value, image] : render_images_) {
            if (!image.pixels || !next->RegisterImage({value}, image.pixels)) {
                return false;
            }
            QueueImageUpload({value});
        }
        renderer_ = std::move(next);
    }
    return renderer_->Ready();
}

bool ClientRenderOwner::ImagesUploaded(const runtime::FramePacket &frame) const
{
    if (!renderer_ || !frame.image_uses) {
        return false;
    }
    for (const auto &use : *frame.image_uses) {
        const auto found = render_images_.find(use.id.value);
        if (found == render_images_.end() || found->second.version != use ||
            !renderer_->ImageUploaded(use.id)) {
            return false;
        }
    }
    return true;
}

bool ClientRenderOwner::AdvanceImageUploads()
{
    if (upload_queue_.empty() || !window_.IsConfigured()) {
        return true;
    }

    const auto metrics = window_.Metrics();
    if (!EnsureRenderer(static_cast<int>(metrics.buffer_size.width),
                        static_cast<int>(metrics.buffer_size.height))) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + config_.install_limits.cpu_per_turn;
    std::size_t uploaded = 0;
    std::uint64_t bytes = 0;
    while (!upload_queue_.empty() && uploaded < config_.install_limits.images_per_turn &&
           std::chrono::steady_clock::now() < deadline) {
        const auto id = upload_queue_.front();
        const auto found = render_images_.find(id.value);
        if (found == render_images_.end() || !found->second.pixels) {
            upload_queue_.pop_front();
            queued_uploads_.erase(id.value);
            continue;
        }
        const auto size = found->second.pixels->rgba.size();
        if (uploaded && size > config_.install_limits.upload_bytes_per_turn -
                                   std::min<std::uint64_t>(
                                       bytes, config_.install_limits.upload_bytes_per_turn)) {
            break;
        }

        if (!renderer_->ImageUploaded(id)) {
            if (!renderer_->UploadImage(id) || !renderer_->ImageUploaded(id)) {
                return false;
            }
            ++uploaded;
            bytes += size;
            ++root_target_.backend_stats.image_uploads;
            root_target_.backend_stats.upload_bytes += size;
            root_target_.backend_stats.oversized_uploads +=
                size > config_.install_limits.upload_bytes_per_turn;
        }
        if (!QueueEvent(runtime::RenderEvent(runtime::ImageUploadedEvent{found->second.version}))) {
            return false;
        }
        upload_queue_.pop_front();
        queued_uploads_.erase(id.value);
    }
    if (!upload_queue_.empty()) {
        window_.RequestUpdate(true);
    }
    return true;
}

} // namespace prism::sdk
