#include "presentation-time-client-protocol.h"
#include "prism/platform/wayland_popup.hpp"
#include "prism/platform/wayland_window.hpp"

#include <limits>
#include <utility>
#include <wayland-client.h>

namespace prism::platform {
WaylandPopupTarget WaylandPopup::Target() const noexcept
{
    return IsConfigured() ? WaylandPopupTarget{surface_lifetime_id_, configure_->generation}
                          : WaylandPopupTarget{};
}

bool WaylandPopup::Matches(const WaylandPopupTarget &target) const noexcept
{
    return target && target == Target() && parent_ && !parent_->closing_ &&
           !parent_->deferred_close_ && !parent_->failed_;
}

PixelSubmissionId WaylandPopup::LastPixelSubmission() const noexcept
{
    return last_pixel_submission_;
}

bool WaylandPopup::FrameCallbackPending() const noexcept
{
    return frame_callback_ != nullptr;
}

bool WaylandPopup::PresentationPending(PixelSubmissionId submission) const noexcept
{
    if (!submission) {
        return false;
    }
    for (const auto &pending : feedbacks_) {
        if (pending.submission == submission) {
            return true;
        }
    }
    return false;
}

bool WaylandPopup::PresentationCapacityAvailable() const noexcept
{
    if (!parent_->presentation_) {
        return true;
    }
    for (const auto &pending : feedbacks_) {
        if (!pending.submission) {
            return true;
        }
    }
    return false;
}

bool WaylandPopup::TrackPresentation(PixelSubmissionId submission)
{
    if (!parent_->presentation_) {
        return true;
    }
    for (auto &pending : feedbacks_) {
        if (pending.submission) {
            continue;
        }
        auto *feedback = wp_presentation_feedback(parent_->presentation_, surface_);
        if (!feedback) {
            return false;
        }
        static const wp_presentation_feedback_listener listener{.sync_output = PresentationOutput,
                                                                .presented = PresentationDone,
                                                                .discarded = PresentationDiscarded};
        if (wp_presentation_feedback_add_listener(feedback, &listener, this) < 0) {
            wp_presentation_feedback_destroy(feedback);
            return false;
        }
        pending = {feedback, Target(), submission, false};
        return true;
    }
    return false;
}

SubmitResult WaylandPopup::SubmitPixels(const WaylandPopupTarget &expected,
                                        const std::function<bool()> &commit_pixels)
{
    if (!Matches(expected) || !buffer_layout_ || !buffer_layout_applied_ || !commit_pixels ||
        submitting_) {
        return SubmitResult::None;
    }
    if (frame_callback_ || !PresentationCapacityAvailable()) {
        return SubmitResult::AwaitFrame;
    }
    if (last_pixel_submission_.value == std::numeric_limits<std::uint64_t>::max()) {
        Close(WaylandPopupCloseReason::ProtocolFailure);
        return SubmitResult::Failed;
    }

    const PixelSubmissionId submission{last_pixel_submission_.value + 1};
    frame_callback_ = wl_surface_frame(surface_);
    if (!frame_callback_) {
        Close(WaylandPopupCloseReason::ProtocolFailure);
        return SubmitResult::Failed;
    }
    frame_target_ = expected;
    frame_submission_ = submission;
    static const wl_callback_listener listener{.done = FrameDone};
    if (wl_callback_add_listener(frame_callback_, &listener, this) < 0 ||
        !TrackPresentation(submission)) {
        Close(WaylandPopupCloseReason::ProtocolFailure);
        return SubmitResult::Failed;
    }

    const auto alive = callback_alive_;
    bool committed{};
    submitting_ = true;
    try {
        committed = commit_pixels();
    } catch (...) {
        committed = false;
    }
    if (!*alive) {
        return SubmitResult::Failed;
    }
    submitting_ = false;
    if (!committed || !Matches(expected)) {
        Close(WaylandPopupCloseReason::ProtocolFailure);
        return SubmitResult::Failed;
    }

    last_pixel_submission_ = submission;
    committed_target_ = expected;
    committed_buffer_layout_ = buffer_layout_;
    state_pending_ = false;
    for (auto &pending : feedbacks_) {
        if (pending.submission == submission) {
            pending.committed = true;
        }
    }
    return SubmitResult::Pixels;
}

SubmitResult WaylandPopup::SubmitState(const WaylandPopupTarget &expected)
{
    if (!Matches(expected) || committed_target_ != expected || !last_pixel_submission_ ||
        !committed_buffer_layout_ || !buffer_layout_ ||
        *committed_buffer_layout_ != *buffer_layout_ || submitting_) {
        return SubmitResult::AwaitFrame;
    }
    if (!state_pending_) {
        return SubmitResult::None;
    }

    wl_surface_commit(surface_);
    state_pending_ = false;
    return SubmitResult::State;
}

bool WaylandPopup::CommitAttachedBuffer()
{
    wl_surface_attach(surface_, attached_buffer_, 0, 0);
    wl_surface_damage_buffer(surface_, 0, 0, attached_width_, attached_height_);
    wl_surface_commit(surface_);
    return true;
}

void WaylandPopup::FrameDone(void *data, wl_callback *callback, std::uint32_t)
{
    auto &self = *static_cast<WaylandPopup *>(data);
    if (callback != self.frame_callback_) {
        return;
    }
    wl_callback_destroy(callback);
    self.frame_callback_ = nullptr;
    const auto target = std::exchange(self.frame_target_, {});
    const auto submission = std::exchange(self.frame_submission_, {});
    if (self.Matches(target)) {
        self.Emit(WaylandPopupFrameReady{target, submission});
    }
}

void WaylandPopup::PresentationOutput(void *, struct wp_presentation_feedback *, wl_output *)
{
}

void WaylandPopup::FinishPresentation(struct wp_presentation_feedback *feedback,
                                      PresentationOutcome outcome)
{
    for (auto &pending : feedbacks_) {
        if (pending.handle != feedback) {
            continue;
        }
        const WaylandPopupPresentation event{pending.target, {pending.submission, outcome}};
        const bool committed = pending.committed;
        wp_presentation_feedback_destroy(feedback);
        pending = {};
        if (!committed || !Matches(event.target)) {
            return;
        }
        if (outcome == PresentationOutcome::Discarded && frame_callback_ &&
            frame_submission_ == event.presentation.submission) {
            wl_callback_destroy(frame_callback_);
            frame_callback_ = nullptr;
            frame_target_ = {};
            frame_submission_ = {};
        }
        EmitPresentation(event);
        return;
    }
}

void WaylandPopup::PresentationDone(void *data, struct wp_presentation_feedback *feedback,
                                    std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                    std::uint32_t, std::uint32_t, std::uint32_t)
{
    static_cast<WaylandPopup *>(data)->FinishPresentation(feedback, PresentationOutcome::Presented);
}

void WaylandPopup::PresentationDiscarded(void *data, struct wp_presentation_feedback *feedback)
{
    static_cast<WaylandPopup *>(data)->FinishPresentation(feedback, PresentationOutcome::Discarded);
}

void WaylandPopup::SetPresentationHandler(
    std::function<void(const WaylandPopupPresentation &)> handler)
{
    presentation_handler_ = std::move(handler);
}

void WaylandPopup::EmitPresentation(const WaylandPopupPresentation &event) noexcept
{
    const auto alive = callback_alive_;
    const auto lifetime = lifetime_generation_;
    try {
        const auto handler = presentation_handler_;
        if (handler) {
            handler(event);
        }
    } catch (...) {
        if (*alive && lifetime == lifetime_generation_) {
            Close(WaylandPopupCloseReason::ProtocolFailure);
        }
    }
}

WaylandPopup::RetiredSubmission WaylandPopup::RetireSubmission() noexcept
{
    RetiredSubmission retired;
    retired.inputs = TakeInputCancellation();
    if (frame_callback_) {
        wl_callback_destroy(frame_callback_);
    }
    frame_callback_ = nullptr;
    frame_target_ = {};
    frame_submission_ = {};
    committed_buffer_layout_.reset();
    state_pending_ = false;
    const auto pending_feedbacks = std::exchange(feedbacks_, {});
    for (std::size_t i = 0; i < pending_feedbacks.size(); ++i) {
        const auto &pending = pending_feedbacks[i];
        if (pending.handle) {
            wp_presentation_feedback_destroy(pending.handle);
        }
        if (pending.submission && pending.committed) {
            retired.presentations[i] = WaylandPopupPresentation{
                pending.target, {pending.submission, PresentationOutcome::Discarded}};
        }
    }
    return retired;
}

void WaylandPopup::DeliverRetiredSubmission(const RetiredSubmission &retired) noexcept
{
    const auto alive = callback_alive_;
    for (const auto &input : retired.inputs) {
        if (input) {
            EmitInput(*input);
            if (!*alive) {
                return;
            }
        }
    }
    for (const auto &presentation : retired.presentations) {
        if (presentation) {
            EmitPresentation(*presentation);
            if (!*alive) {
                return;
            }
        }
    }
}

void WaylandPopup::RevokeSubmission() noexcept
{
    const auto retired = RetireSubmission();
    DeliverRetiredSubmission(retired);
}
} // namespace prism::platform
