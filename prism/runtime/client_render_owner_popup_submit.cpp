#include "client_render_owner_p.hpp"

#include <cstdio>
#include <functional>
#include <limits>
#include <stdexcept>

namespace prism::sdk {

bool ClientRenderOwner::PopupImagesUploaded(const runtime::PopupFramePacket &frame)
{
    if (!renderer_ || !frame.image_uses) {
        return false;
    }
    bool ready = true;
    for (const auto &use : *frame.image_uses) {
        const auto found = render_images_.find(use.id.value);
        if (found != render_images_.end() && found->second.version.generation > use.generation) {
            // A replacement resource can arrive before its owning UI packet.
            // Keep the submitted native surface and wait for the new version.
            ready = false;
            continue;
        }
        if (found == render_images_.end() || found->second.version != use) {
            throw std::runtime_error("Popup image version unavailable");
        }
        if (!renderer_->ImageUploaded(use.id)) {
            QueueImageUpload(use.id);
            ready = false;
        }
    }
    return ready;
}

bool ClientRenderOwner::AdvancePopup()
{
    if (terminal_.StopRequested() || terminal_.Reason() != runtime::TerminalReason::None) {
        return true;
    }
    ReconcilePopup();
    if (!popup_frame_ || !PopupFrameMatches(*popup_frame_) ||
        processed_event_sequence_ != issued_event_sequence_ || commands_.Size()) {
        return true;
    }
    const auto frame = popup_frame_;
    const bool effects_ready = popup_clean_parent_;
    if (popup_effects_ready_ == effects_ready && !popup_force_pixels_ && popup_committed_frame_ &&
        frame->sequence <= popup_committed_frame_->sequence) {
        if (frame == popup_committed_frame_) {
            ConsumeApprovedPopup(frame);
        }
        if (animation_sampling_active_ && !window_.FrameCallbackPending() &&
            !popup_.FrameCallbackPending() && !frame_opportunity_ && !approved_frame_ &&
            !spontaneous_animation_frame_allowed_) {
            return IssueFrameOpportunity();
        }
        return true;
    }

    try {
        const auto &plan = *frame->plan;
        const auto size = plan.buffer_size;
        if (!EnsureRenderer(int(window_.Metrics().buffer_size.width),
                            int(window_.Metrics().buffer_size.height)) ||
            !PopupImagesUploaded(*frame)) {
            return true;
        }
        const bool pixels =
            popup_force_pixels_ || !popup_committed_frame_ ||
            plan.buffer_size.width != popup_committed_frame_->plan->buffer_size.width ||
            plan.buffer_size.height != popup_committed_frame_->plan->buffer_size.height ||
            plan.window_geometry != popup_committed_frame_->plan->window_geometry ||
            plan.display_list->commands != popup_committed_frame_->plan->display_list->commands ||
            frame->resource_epoch != popup_committed_frame_->resource_epoch;
        if (pixels && popup_.FrameCallbackPending()) {
            return true;
        }
        if (pixels && animation_sampling_active_ && frame_opportunity_) {
            return true;
        }
        if (pixels && animation_sampling_active_ &&
            (!approved_frame_ || approved_frame_->popup_surface_frame != frame)) {
            if (!window_.FrameCallbackPending() && !frame_opportunity_ &&
                !spontaneous_animation_frame_allowed_ && !popup_.FrameCallbackPending()) {
                return IssueFrameOpportunity();
            }
            return true;
        }
        const platform::WaylandPopupBufferLayout layout{size, plan.window_geometry,
                                                        plan.configure_generation};
        if (!popup_.SetBufferLayout(layout)) {
            throw std::runtime_error("Popup layout no longer matches configure");
        }
        if (!popup_.SetInputRegions(popup_.Target(), plan.input_regions)) {
            throw std::runtime_error("Popup input region rejected");
        }

        const auto capability = popup_.SurfaceEffectCapabilities();
        for (const auto &effect : plan.effect_regions) {
            if (!capability.backdrop || !capability.popup_backdrop ||
                (effect.contour && !capability.contour)) {
                throw std::runtime_error("Popup backdrop capability unavailable");
            }
        }
        const std::span<const contracts::SurfaceEffectRegion> effects =
            effects_ready ? std::span<const contracts::SurfaceEffectRegion>(plan.effect_regions)
                          : std::span<const contracts::SurfaceEffectRegion>{};
        if (!popup_.SetSurfaceEffects(popup_.Target(), effects)) {
            throw std::runtime_error("Popup surface effects rejected");
        }

        platform::SubmitResult result;
        if (pixels) {
            result = popup_.SubmitPixels(
                popup_.Target(), std::bind_front(&ClientRenderOwner::CommitPopupPixels, this));
        } else {
            result = popup_.SubmitState(popup_.Target());
        }
        if (result == platform::SubmitResult::AwaitFrame ||
            result == platform::SubmitResult::Deferred) {
            return true;
        }
        if (result == platform::SubmitResult::Failed || popup_.IsClosed()) {
            ClosePopup(true);
            return true;
        }
        if (next_popup_adoption_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Popup adoption sequence exhausted");
        }
        const bool submitted_pixels = result == platform::SubmitResult::Pixels;
        popup_input_identity_ = frame->identity;
        popup_input_identity_.submission_sequence = ++next_popup_adoption_sequence_;
        popup_committed_frame_ = frame;
        popup_effects_ready_ = effects_ready;
        popup_target_.input_ui = frame->ui;
        popup_target_.input_snapshot = plan.input_snapshot;
        if (submitted_pixels) {
            ++popup_pixel_commits_;
            popup_force_pixels_ = false;
        }
        const auto submission = popup_.LastPixelSubmission();
        const runtime::PopupSubmittedEvent submitted{frame->ui,
                                                     popup_input_identity_,
                                                     frame->sequence,
                                                     frame,
                                                     submitted_pixels &&
                                                         popup_.PresentationPending(submission),
                                                     submitted_pixels,
                                                     submission};
        if (!QueueEvent(runtime::RenderEvent(submitted))) {
            return false;
        }
        ConsumeApprovedPopup(frame);
        return true;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[prism-sdk] popup fallback: %s\n", error.what());
        ClosePopup(true);
        return true;
    }
}

bool ClientRenderOwner::CommitPopupPixels()
{
    const auto frame = popup_frame_;
    if (!frame || !PopupFrameMatches(*frame)) {
        return false;
    }
    const auto &plan = *frame->plan;
    const auto size = plan.buffer_size;
    const int width = int(size.width);
    const int height = int(size.height);
    if (!popup_target_.egl.Ready()) {
        if (!popup_target_.egl.Open(egl_context_, popup_.Surface(), width, height)) {
            return false;
        }
        popup_target_.damage_history.Reset(size);
    }
    if (!popup_target_.egl.Resize(width, height) || !popup_target_.egl.MakeCurrent()) {
        return false;
    }
    const auto old_size = popup_target_.damage_history.Size();
    const bool resized = old_size.width != size.width || old_size.height != size.height;
    if (resized) {
        popup_target_.damage_history.Reset(size);
    }
    const auto content =
        resized || popup_force_pixels_ || !popup_committed_frame_
            ? contracts::DamageRegion::Full()
            : damage_commands_->CompareDamage(popup_committed_frame_->plan->display_list.get(),
                                              *plan.display_list, width, height,
                                              popup_target_.committed_damage_resource_epoch);
    const auto age = popup_target_.egl.QueryBufferAge();
    auto damage = popup_target_.damage_history.Plan(
        content, age && *age >= 0 ? std::optional<unsigned>(unsigned(*age)) : std::nullopt);
    if (!config_.partial_rendering) {
        damage.repair_damage = contracts::DamageRegion::Full();
    }
    const auto identity = popup_target_.egl.TargetIdentity();
    if (popup_target_.egl.SetDamage(damage.repair_damage) == platform::DamageRegionResult::Failed ||
        !renderer_->Render(*plan.display_list, identity, width, height, damage.repair_damage) ||
        identity != popup_target_.egl.TargetIdentity() || !PopupFrameMatches(*frame) ||
        !popup_target_.egl.Swap(damage.content_damage)) {
        return false;
    }
    if (!popup_target_.damage_history.Commit(std::move(damage))) {
        return false;
    }
    popup_target_.committed_damage_resource_epoch = damage_commands_->ResourceEpoch();
    return true;
}
} // namespace prism::sdk
