#include "client_application_p.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace prism::sdk {
namespace {
void PushAnimationCommand(ClientRenderBridge &bridge, runtime::RenderCommand command)
{
    if (bridge.render_commands.TryPush(std::move(command)) == runtime::QueuePushResult::Accepted) {
        return;
    }

    bridge.terminal.Fail(runtime::TerminalReason::CommandQueueFailure);
    throw std::runtime_error("Animation render command queue unavailable");
}

bool SameImageUses(const std::shared_ptr<const std::vector<runtime::ImageVersion>> &a,
                   const std::shared_ptr<const std::vector<runtime::ImageVersion>> &b) noexcept
{
    return a == b || (a && b && *a == *b);
}
} // namespace

bool ClientApplication::Impl::HasUnsubmittedPopupPixels() const noexcept
{
    const auto next = queued_frame ? queued_frame->popup_surface_frame : nullptr;
    if (!next || !next->plan || !next->plan->display_list) {
        return false;
    }
    const auto old = ui_popup_submitted_frame;
    if (!old || !old->plan || !old->plan->display_list) {
        return true;
    }

    return next->ui != old->ui || next->worker != old->worker ||
           next->identity.worker != old->identity.worker ||
           next->identity.target != old->identity.target ||
           next->identity.lifetime != old->identity.lifetime ||
           next->identity.configure_generation != old->identity.configure_generation ||
           next->resource_epoch != old->resource_epoch ||
           next->plan->buffer_size.width != old->plan->buffer_size.width ||
           next->plan->buffer_size.height != old->plan->buffer_size.height ||
           next->plan->window_geometry != old->plan->window_geometry ||
           next->plan->display_list->commands != old->plan->display_list->commands ||
           !SameImageUses(next->image_uses, old->image_uses);
}

bool ClientApplication::Impl::HasUnsubmittedPixels() const noexcept
{
    if (HasUnsubmittedPopupPixels()) {
        return true;
    }
    if (!queued_frame || !queued_frame->display_list) {
        return false;
    }
    if (!ui_submitted_frame) {
        return true;
    }

    const auto metadata = ui_root_metadata_frame ? ui_root_metadata_frame : ui_submitted_frame;
    return queued_frame->ui != ui_submitted_frame->ui ||
           queued_frame->configure_count != metadata->configure_count ||
           queued_frame->display_list != ui_submitted_frame->display_list ||
           queued_frame->resource_epoch != ui_submitted_frame->resource_epoch ||
           !SameImageUses(queued_frame->image_uses, ui_submitted_frame->image_uses) ||
           queued_frame->buffer_size.width != ui_submitted_frame->buffer_size.width ||
           queued_frame->buffer_size.height != ui_submitted_frame->buffer_size.height ||
           queued_frame->scale != ui_submitted_frame->scale;
}

void ClientApplication::Impl::SyncAnimationSampling()
{
    const bool active = HasActiveAnimations();
    if (active) {
        animation_deadline_ns.reset();
        pending_animation_finish_sequence.reset();
    } else {
        animation_deadline_ns.reset();
        if (animation_worker_active && (HasUnsubmittedPixels() || HasPendingTaskMotionEndpoint())) {
            pending_animation_finish_sequence = queued_frame->sequence;
            return;
        }
        pending_animation_finish_sequence.reset();
    }

    if (active == animation_worker_active || !installed_ui.owner || !installed_ui.generation) {
        return;
    }

    PushAnimationCommand(*bridge, runtime::RenderCommand(
                                      runtime::SetAnimationSamplingCommand{installed_ui, active}));
    animation_worker_active = active;
    if (active) {
        QueueRenderUpdate(true);
    }
}

void ClientApplication::Impl::HandleFrameOpportunity(const runtime::FrameOpportunityEvent &event)
{
    if (!scene || !animation_worker_active || event.ui != installed_ui || !worker_generation ||
        event.worker != *worker_generation || event.configure_count != ui_configure_count ||
        !event.id) {
        return;
    }

    animation_deadline_ns.reset();
    const auto now = scene->AnimationNowNs();
    if (AdvanceAnimations(now)) {
        InvalidateQueuedFrame();
    }

    PublishFramePacket();
    auto frame = HasUnsubmittedPixels() || HasPendingTaskMotionEndpoint() ? queued_frame : nullptr;
    PushAnimationCommand(*bridge,
                         runtime::RenderCommand(runtime::AnswerFrameOpportunityCommand{
                             event.ui, event.worker, event.configure_count, event.id, frame}));

    if (HasActiveAnimations()) {
        if (!frame) {
            animation_deadline_ns = NextAnimationDeadlineNs(now);
        }
    } else {
        SyncAnimationSampling();
    }
}

void ClientApplication::Impl::AdvanceAnimationDeadline()
{
    if (!scene || !animation_deadline_ns) {
        return;
    }

    const auto now = scene->AnimationNowNs();
    if (now < *animation_deadline_ns) {
        return;
    }

    animation_deadline_ns.reset();
    const bool changed = AdvanceAnimations(now);
    if (changed) {
        InvalidateQueuedFrame();
        PublishFramePacket();
    }

    if (HasActiveAnimations()) {
        if (!changed) {
            animation_deadline_ns = NextAnimationDeadlineNs(now);
        }
    } else {
        SyncAnimationSampling();
    }
}

int ClientApplication::Impl::AnimationTimeoutMs(int timeout_ms) const noexcept
{
    if (!scene || !animation_deadline_ns) {
        return timeout_ms;
    }

    const auto now = scene->AnimationNowNs();
    if (now >= *animation_deadline_ns) {
        return 0;
    }

    const auto remaining_ns = *animation_deadline_ns - now;
    const auto rounded_ms = remaining_ns / 1'000'000 + (remaining_ns % 1'000'000 != 0);
    const auto bounded = static_cast<int>(std::min<std::uint64_t>(
        rounded_ms, static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
    return timeout_ms < 0 ? bounded : std::min(timeout_ms, bounded);
}
} // namespace prism::sdk
