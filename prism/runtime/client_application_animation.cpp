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
} // namespace

bool ClientApplication::Impl::HasUnsubmittedPixels() const noexcept
{
    if (!queued_frame || !queued_frame->display_list) {
        return false;
    }
    if (!ui_submitted_frame) {
        return true;
    }

    return queued_frame->ui != ui_submitted_frame->ui ||
           queued_frame->configure_count != ui_submitted_frame->configure_count ||
           queued_frame->pixels_revision != ui_submitted_frame->pixels_revision ||
           queued_frame->buffer_size.width != ui_submitted_frame->buffer_size.width ||
           queued_frame->buffer_size.height != ui_submitted_frame->buffer_size.height ||
           queued_frame->scale != ui_submitted_frame->scale;
}

void ClientApplication::Impl::SyncAnimationSampling()
{
    const bool active = scene && scene->HasActiveAnimations();
    if (active) {
        animation_deadline_ns.reset();
        pending_animation_finish_sequence.reset();
    } else {
        animation_deadline_ns.reset();
        if (animation_worker_active && HasUnsubmittedPixels()) {
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
    if (scene->AdvanceAnimations(now)) {
        InvalidateQueuedFrame();
    }

    PublishFramePacket();
    auto frame = HasUnsubmittedPixels() ? queued_frame : nullptr;
    PushAnimationCommand(*bridge,
                         runtime::RenderCommand(runtime::AnswerFrameOpportunityCommand{
                             event.ui, event.worker, event.configure_count, event.id, frame}));

    if (scene->HasActiveAnimations()) {
        if (!frame) {
            animation_deadline_ns = scene->NextAnimationDeadlineNs(now);
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
    const bool changed = scene->AdvanceAnimations(now);
    if (changed) {
        InvalidateQueuedFrame();
        PublishFramePacket();
    }

    if (scene->HasActiveAnimations()) {
        if (!changed) {
            animation_deadline_ns = scene->NextAnimationDeadlineNs(now);
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
