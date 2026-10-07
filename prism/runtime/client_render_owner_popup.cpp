#include "client_render_owner_p.hpp"
#include "popup_backdrop_barrier_p.hpp"

#include <cstdio>
#include <functional>
#include <limits>
#include <utility>

namespace prism::sdk {
namespace {
bool SamePopup(const runtime::PopupSurfaceRequest &a, const runtime::PopupSurfaceRequest &b)
{
    return a.scene == b.scene && a.popup_token == b.popup_token && a.active_node == b.active_node &&
           a.trigger == b.trigger &&
           a.parent_configure_generation == b.parent_configure_generation &&
           a.parent_window_geometry == b.parent_window_geometry && a.anchor == b.anchor &&
           a.desired_geometry == b.desired_geometry && a.gap == b.gap &&
           a.horizontal_alignment == b.horizontal_alignment &&
           a.vertical_preference == b.vertical_preference;
}
} // namespace

runtime::PopupSurfaceIdentity
ClientRenderOwner::PopupIdentity(platform::WaylandPopupTarget target) const noexcept
{
    return {worker_generation_.value, target.surface_lifetime_id, target.surface_lifetime_id,
            target.configure_generation, 0};
}

void ClientRenderOwner::ReconcilePopup()
{
    const auto frame = root_target_.render_frame;
    if (!frame) {
        // Invalidating a candidate is not a logical close. Keep the successful
        // child/input baseline while the UI prepares the replacement packet.
        if (!popup_.IsClosed() && popup_request_ &&
            popup_request_->parent_configure_generation !=
                std::uint64_t(window_.ConfigureCount())) {
            ClosePopup();
        }
        return;
    }
    const auto request = frame ? frame->popup_surface_request : nullptr;
    if (!frame || !request || frame->ui != installed_ui_ || frame->scale != 1 ||
        (request->requires_backdrop && (!window_.SurfaceEffectCapabilities().backdrop ||
                                        !window_.SurfaceEffectCapabilities().popup_backdrop)) ||
        request->parent_configure_generation != std::uint64_t(window_.ConfigureCount())) {
        if (!popup_.IsClosed()) {
            ClosePopup();
        }
        return;
    }
    if (blocked_popup_request_ && SamePopup(*blocked_popup_request_, *request) &&
        blocked_popup_request_->theme_generation == request->theme_generation) {
        return;
    }
    blocked_popup_request_.reset();
    if (!popup_.IsClosed() &&
        (popup_ui_ != frame->ui || !popup_request_ || !SamePopup(*popup_request_, *request))) {
        ClosePopup();
    }
    if (popup_.IsClosed()) {
        // Open also checks the parent's current size against its actual pixel
        // baseline. A same-size configure can be satisfied by metadata alone.
        if (!window_.IsMapped() || !root_target_.committed_frame) {
            return;
        }
        popup_ui_ = frame->ui;
        popup_request_ = request;
        popup_.SetEventHandler(std::bind_front(&ClientRenderOwner::QueuePopupEvent, this));
        popup_.SetInputHandler(std::bind_front(&ClientRenderOwner::QueuePopupInput, this));
        popup_.SetPresentationHandler(
            std::bind_front(&ClientRenderOwner::QueuePopupPresentation, this));
        popup_.SetBeforeCloseHandler(std::bind_front(&ClientRenderOwner::ReleasePopupGpu, this));
        if (!popup_.Open(window_, {request->anchor, request->desired_geometry, request->gap,
                                   request->horizontal_alignment, request->vertical_preference})) {
            blocked_popup_request_ = request;
            popup_request_.reset();
            return;
        }
    }
    popup_request_ = request;
    const auto candidate = frame->popup_surface_frame;
    if (candidate && PopupFrameMatches(*candidate)) {
        popup_frame_ = candidate;
    } else {
        popup_frame_.reset();
    }
}

bool ClientRenderOwner::PopupFrameMatches(const runtime::PopupFramePacket &frame) const noexcept
{
    const auto target = popup_.Target();
    return target && frame.ui == installed_ui_ && frame.ui == popup_ui_ &&
           frame.worker == worker_generation_ && frame.identity == PopupIdentity(target) &&
           frame.plan && frame.image_uses && popup_request_ &&
           frame.plan->configure_generation == target.configure_generation &&
           frame.plan->request == *popup_request_;
}

void ClientRenderOwner::ClosePopup(bool block) noexcept
{
    if (block) {
        blocked_popup_request_ = popup_request_;
    }
    popup_.Close();
    ReleasePopupGpu();
    popup_request_.reset();
    popup_frame_.reset();
    popup_committed_frame_.reset();
    popup_input_identity_ = {};
    popup_clean_parent_ = false;
    popup_effects_ready_ = false;
    if (animation_sampling_active_) {
        ResetFrameOpportunity();
    }
}

void ClientRenderOwner::ReleasePopupGpu() noexcept
{
    if (renderer_ && popup_target_.egl.Ready()) {
        if (root_target_.egl.MakeCurrent() || popup_target_.egl.MakeCurrent()) {
            renderer_->ReleaseTarget(popup_target_.egl.TargetIdentity());
        } else {
            renderer_->Abandon();
        }
    }
    popup_target_.egl.Close();
    popup_target_.ResetSubmission();
    popup_force_pixels_ = false;
}

void ClientRenderOwner::QueuePopupEvent(const platform::WaylandPopupEvent &event) noexcept
{
    try {
        if (const auto *ready = std::get_if<platform::WaylandPopupFrameReady>(&event)) {
            if (ready->target == popup_.Target() && animation_sampling_active_ &&
                !window_.FrameCallbackPending() && !popup_.FrameCallbackPending() &&
                !frame_opportunity_ && !approved_frame_ && !spontaneous_animation_frame_allowed_) {
                IssueFrameOpportunity();
            }
            return;
        }
        if (!popup_request_ ||
            issued_event_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
            terminal_.Fail(runtime::TerminalReason::InternalFailure);
            return;
        }
        if (const auto *configure = std::get_if<platform::WaylandPopupConfigure>(&event)) {
            popup_frame_.reset();
            popup_committed_frame_.reset();
            popup_target_.ResetSubmission();
            popup_input_identity_ = {};
            popup_clean_parent_ = false;
            popup_effects_ready_ = false;
            ++popup_configures_;
            const runtime::PopupConfigureEvent copy{popup_ui_,
                                                    PopupIdentity(popup_.Target()),
                                                    ++issued_event_sequence_,
                                                    popup_request_,
                                                    {popup_request_->parent_configure_generation,
                                                     configure->generation, configure->bounds}};
            QueueEvent(runtime::RenderEvent(copy));
            return;
        }
        if (const auto *closed = std::get_if<platform::WaylandPopupClosed>(&event)) {
            ++popup_closes_;
            const runtime::PopupClosedEvent copy{popup_ui_, PopupIdentity(closed->target),
                                                 ++issued_event_sequence_, popup_request_,
                                                 closed->reason};
            QueueEvent(runtime::RenderEvent(copy));
            popup_committed_frame_.reset();
            popup_input_identity_ = {};
            popup_clean_parent_ = false;
            popup_effects_ready_ = false;
        }
    } catch (...) {
        terminal_.Fail(runtime::TerminalReason::EventQueueFailure);
    }
}

void ClientRenderOwner::QueuePopupInput(const platform::WaylandPopupInput &input) noexcept
{
    const auto *focus = std::get_if<contracts::FocusEvent>(&input.event);
    const bool cancel = std::holds_alternative<contracts::PointerCancelEvent>(input.event) ||
                        (focus && !focus->focused);
    const auto expected = platform::WaylandPopupTarget{popup_input_identity_.lifetime,
                                                       popup_input_identity_.configure_generation};
    if (!popup_committed_frame_ || input.target != expected ||
        (!cancel && input.target != popup_.Target()) ||
        input.submission != popup_.LastPixelSubmission() ||
        !popup_input_identity_.submission_sequence) {
        return;
    }
    try {
        if (issued_event_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
            terminal_.Fail(runtime::TerminalReason::InternalFailure);
            return;
        }
        const auto sequence = issued_event_sequence_ + 1;
        runtime::RenderEvent event(
            runtime::PopupInputEvent{popup_ui_, popup_input_identity_, sequence, input.event,
                                     popup_committed_frame_->plan->input_snapshot});
        const auto result =
            events_.TryPushLatest(std::move(event), runtime::ReplacePointerMotionTail);
        if (result == runtime::QueuePushResult::Accepted ||
            result == runtime::QueuePushResult::Replaced) {
            issued_event_sequence_ = sequence;
        } else {
            terminal_.Fail(runtime::TerminalReason::EventQueueFailure);
        }
    } catch (...) {
        terminal_.Fail(runtime::TerminalReason::EventQueueFailure);
    }
}

void ClientRenderOwner::QueuePopupPresentation(
    const platform::WaylandPopupPresentation &event) noexcept
{
    const auto expected = platform::WaylandPopupTarget{popup_input_identity_.lifetime,
                                                       popup_input_identity_.configure_generation};
    if (event.target != popup_.Target() && event.target != expected) {
        return;
    }
    if (event.presentation.outcome == platform::PresentationOutcome::Discarded &&
        event.presentation.submission == popup_.LastPixelSubmission() &&
        event.target == popup_.Target()) {
        popup_force_pixels_ = true;
    }
    QueueEvent(runtime::RenderEvent(runtime::PopupPresentationEvent{
        popup_ui_, PopupIdentity(event.target), event.presentation}));
}

void ClientRenderOwner::RetirePopupImage(runtime::ImageVersion image)
{
    for (const auto &frame : {popup_committed_frame_, popup_frame_}) {
        if (frame && frame->image_uses) {
            for (const auto &use : *frame->image_uses) {
                if (use == image) {
                    ClosePopup(true);
                    return;
                }
            }
        }
    }
}

void ClientRenderOwner::AdoptPopupCleanParent(const runtime::FramePacket &frame) noexcept
{
    const auto current = PopupIdentity(popup_.Target());
    if (popup_clean_parent_ || current.worker != popup_input_identity_.worker ||
        current.target != popup_input_identity_.target ||
        current.lifetime != popup_input_identity_.lifetime ||
        current.configure_generation != popup_input_identity_.configure_generation ||
        !CanEnablePopupBackdrop(frame, root_target_.committed_frame.get(), popup_input_identity_,
                                popup_ui_, window_.ConfigureCount())) {
        return;
    }

    popup_clean_parent_ = true;
    // A checked root None need not produce a native callback. Wake exactly
    // once so the next owner turn installs child effects even if input is idle.
    try {
        runtime::RenderCommand wake(
            runtime::RequestRenderCommand{runtime::RenderRequestKind::Update, true});
        const auto queued = commands_.TryPush(std::move(wake));
        if (queued != runtime::QueuePushResult::Accepted &&
            queued != runtime::QueuePushResult::Busy) {
            terminal_.Fail(runtime::TerminalReason::CommandQueueFailure);
        }
    } catch (...) {
        terminal_.Fail(runtime::TerminalReason::CommandQueueFailure);
    }
}
} // namespace prism::sdk
