#include "client_application_p.hpp"

#include <stdexcept>

namespace prism::sdk {
namespace {
runtime::TaskPresentationBinding PresentationBinding(const runtime::FramePacket &packet,
                                                     std::uint64_t token, contracts::NodeId root)
{
    const auto &input = *packet.input_snapshot;
    return {token,
            input.owner_modal_epoch,
            root,
            input.scene,
            input.version,
            packet.configure_count,
            packet.buffer_size.width,
            packet.buffer_size.height,
            packet.scale,
            packet.theme_generation};
}
} // namespace

std::optional<runtime::TaskPresentationState>
ClientApplication::OwnerTaskPresentation() const noexcept
{
    return impl_->owner_task_presentation.Current();
}

void ClientApplication::Impl::InterruptOwnerTaskPresentation(
    runtime::TaskPresentationInterruptReason reason)
{
    owner_task_paint.reset();
    ResetOwnerTaskMotion();
    if (const auto state = owner_task_presentation.Current();
        state && state->phase != runtime::TaskPresentationPhase::Closed) {
        owner_task_presentation.Interrupt(state->identity, reason);
    }
}

void ClientApplication::Impl::InvalidateOwnerTaskPresentation(
    runtime::TaskPresentationInterruptReason reason)
{
    owner_task_paint.reset();
    const auto state = owner_task_presentation.Current();
    if (!state || state->phase == runtime::TaskPresentationPhase::Closed) {
        return;
    }

    if (state->phase == runtime::TaskPresentationPhase::Closing) {
        ResetOwnerTaskMotion();
        owner_task_presentation.Interrupt(state->identity, reason);
        return;
    }
    if (!owner_task_presentation.InvalidateProjection(state->identity)) {
        // An exhausted projection cannot retain an apparently valid endpoint.
        // The normal frontend failure path retires the business scope as well.
        owner_task_presentation.Interrupt(state->identity, reason);
        CancelCurrentOwnerTask(runtime::TaskCancelReason::ScopeUnavailable);
        throw std::overflow_error("Task presentation projection exhausted");
    }
    FallBackOwnerTaskMotion();
}

void ClientApplication::Impl::CaptureOwnerTaskPresentation(runtime::FramePacket &packet)
{
    const auto state = owner_task_presentation.Current();
    if (!state || state->phase == runtime::TaskPresentationPhase::Closed || !scene ||
        state->identity.ui != installed_ui || packet.ui != installed_ui || !packet.input_snapshot ||
        packet.input_snapshot->owner_modal_epoch != scene->OwnerModalEpoch()) {
        return;
    }

    const bool closing = state->phase == runtime::TaskPresentationPhase::Closing;
    if (closing) {
        if (owner_task_scope || scene->OwnerModalToken() ||
            (owner_tasks && owner_tasks->Active())) {
            return;
        }
    } else if (!owner_task_scope || owner_task_scope->presentation != state->identity ||
               owner_task_scope->ui != installed_ui ||
               owner_task_scope->token != scene->OwnerModalToken()) {
        return;
    }

    const auto binding =
        PresentationBinding(packet, closing ? 0 : owner_task_scope->token,
                            closing ? contracts::NodeId{} : owner_task_scope->root);
    packet.task_presentation = owner_task_presentation.Publish(
        state->identity, binding, packet.sequence,
        packet.task_motion ? packet.task_motion->sample.kind
                           : runtime::TaskPresentationSampleKind::Terminal);
    if (!packet.task_presentation) {
        ResetOwnerTaskMotion();
        owner_task_paint.reset();
        owner_task_presentation.Interrupt(
            state->identity, runtime::TaskPresentationInterruptReason::ScopeUnavailable);
        throw std::runtime_error("Task presentation frame could not be published");
    }
}

void ClientApplication::Impl::AdoptOwnerTaskPresentation(const runtime::FramePacket &packet)
{
    if (!packet.task_presentation || !packet.input_snapshot || !scene || closed || failed ||
        owner_tasks_retired || packet.ui != installed_ui ||
        packet.task_presentation->identity.ui != installed_ui ||
        packet.task_presentation->frame_sequence != packet.sequence ||
        packet.configure_count != ui_configure_count ||
        packet.buffer_size.width != ui_metrics.buffer_size.width ||
        packet.buffer_size.height != ui_metrics.buffer_size.height ||
        packet.scale != ui_metrics.scale ||
        packet.theme_generation != (theme ? theme->generation : 0) ||
        packet.resource_epoch != commands.ResourceEpoch() ||
        packet.input_snapshot->owner_modal_epoch != scene->OwnerModalEpoch() ||
        !scene->IsInputSnapshotAdopted(*packet.input_snapshot)) {
        return;
    }

    // An adopted older snapshot is still valid input evidence under the
    // existing business gate, but cannot complete a newer live projection.
    // Resolve no layout here; publication owns layout and pixel preparation.
    if (runtime::Has(scene->PendingDirty(), runtime::Dirty::Layout)) {
        return;
    }
    const auto live_input = scene->CaptureInputSnapshot();
    const auto &input = *packet.input_snapshot;
    if (!live_input || live_input->scene != input.scene || live_input->version != input.version ||
        live_input->root != input.root || live_input->viewport != input.viewport ||
        live_input->owner_modal_epoch != input.owner_modal_epoch) {
        return;
    }

    const auto state = owner_task_presentation.Current();
    const auto &stamp = *packet.task_presentation;
    if (!state || stamp.identity != state->identity) {
        return;
    }
    const bool closing = state->phase == runtime::TaskPresentationPhase::Closing;
    if (closing) {
        if (owner_task_scope || scene->OwnerModalToken() ||
            (owner_tasks && owner_tasks->Active())) {
            return;
        }
    } else if ((state->phase != runtime::TaskPresentationPhase::Opening &&
                state->phase != runtime::TaskPresentationPhase::Open) ||
               !owner_task_scope || owner_task_scope->presentation != state->identity ||
               owner_task_scope->ui != installed_ui ||
               owner_task_scope->token != scene->OwnerModalToken()) {
        return;
    }

    const auto binding =
        PresentationBinding(packet, closing ? 0 : owner_task_scope->token,
                            closing ? contracts::NodeId{} : owner_task_scope->root);
    if (binding == stamp.binding && MatchesOwnerTaskMotionFrame(packet) &&
        owner_task_presentation.Adopt(stamp)) {
        AdoptOwnerTaskMotion(packet);
        AdoptOwnerTaskPaint(packet);
        if (owner_task_scope && IsStandardOwnerTask() &&
            owner_task_presentation.Current()->phase == runtime::TaskPresentationPhase::Open) {
            scene->SetOwnerModalInputReady(owner_task_scope->token, true);
        }
    }
}
} // namespace prism::sdk
