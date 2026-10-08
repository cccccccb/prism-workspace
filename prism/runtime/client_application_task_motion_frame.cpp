#include "client_application_p.hpp"

#include <cmath>

namespace prism::sdk {

void ClientApplication::Impl::ApplyOwnerTaskMotionPaint(runtime::FramePacket &packet)
{
    if (!owner_task_motion || !packet.task_motion || !last_list) {
        return;
    }
    const auto state = owner_task_presentation.Current();
    if (!state || state->identity != owner_task_motion->identity) {
        return;
    }

    const auto reveal = owner_task_motion->current.sample.reveal;
    std::optional<contracts::DisplayList> list;
    bool unsupported = false;
    try {
        if (state->phase == runtime::TaskPresentationPhase::Closing) {
            if (const auto paint = ClosingOwnerTaskPaint(); paint && reveal > 0) {
                list = runtime::ComposeTaskPaint(*last_list, *paint, reveal);
            }
        } else if (!owner_task_motion->fallback && owner_task_scope && reveal < 1) {
            list = scene->CaptureTaskOpacityFrame(owner_task_scope->root, reveal);
            unsupported = !list;
        }

        if (!unsupported) {
            if (!list || list->commands == last_list->commands) {
                packet.display_list = last_list;
            } else if (last_task_motion_list && last_task_motion_list->commands == list->commands) {
                packet.display_list = last_task_motion_list;
            } else {
                packet.display_list =
                    std::make_shared<const contracts::DisplayList>(std::move(*list));
            }
        }
    } catch (const std::exception &) {
        // Optional motion cannot make an otherwise valid task fail to open or close.
        unsupported = true;
    }
    if (unsupported) {
        FallBackOwnerTaskMotion();
        packet.task_motion = owner_task_motion->current;
        packet.display_list = last_list;
    }
    last_task_motion_list = packet.display_list;
}

bool ClientApplication::Impl::MatchesOwnerTaskMotionFrame(const runtime::FramePacket &packet) const
{
    if (!owner_task_motion) {
        return !packet.task_motion;
    }
    if (!packet.task_motion || !packet.task_presentation) {
        return false;
    }
    const auto &frame = *packet.task_motion;
    const auto &current = owner_task_motion->current;
    const auto &sample = frame.sample;
    if (frame.identity != current.identity || frame.generation != current.generation ||
        !frame.revision || frame.revision > current.revision ||
        frame.identity != packet.task_presentation->identity ||
        sample.kind != packet.task_presentation->sample_kind ||
        sample.endpoint != packet.task_presentation->endpoint || !std::isfinite(sample.reveal) ||
        sample.reveal < 0 || sample.reveal > 1 || sample.time_ns > current.sample.time_ns ||
        (frame.revision == current.revision && sample != current.sample)) {
        return false;
    }
    if (sample.kind == runtime::TaskPresentationSampleKind::Terminal) {
        return sample.reveal ==
               (sample.endpoint == runtime::TaskPresentationEndpoint::Open ? 1 : 0);
    }
    // Older samples come from the actual immutable packet consumed by the
    // worker, not external input. Keep valid delayed adoptions without an
    // unbounded per-frame ledger or retaining their former whole frames.
    return true;
}

void ClientApplication::Impl::AdoptOwnerTaskMotion(const runtime::FramePacket &packet)
{
    const auto state = owner_task_presentation.Current();
    if (state && state->phase == runtime::TaskPresentationPhase::Closed) {
        ResetOwnerTaskMotion();
    } else if (owner_task_motion && packet.task_motion) {
        owner_task_motion->adopted = packet.task_motion;
    }
}

bool ClientApplication::Impl::HasPendingTaskMotionEndpoint() const noexcept
{
    const auto state = owner_task_presentation.Current();
    if (!state ||
        (state->phase != runtime::TaskPresentationPhase::Opening &&
         state->phase != runtime::TaskPresentationPhase::Closing) ||
        !queued_frame || !queued_frame->task_presentation) {
        return false;
    }
    const auto &stamp = *queued_frame->task_presentation;
    return stamp.identity == state->identity && stamp.projection == state->projection &&
           stamp.sample_kind == runtime::TaskPresentationSampleKind::Terminal &&
           queued_frame->sequence > state->adopted_sequence;
}

} // namespace prism::sdk
