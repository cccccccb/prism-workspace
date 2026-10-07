#include "client_application_p.hpp"

#include <cstdio>
#include <utility>

namespace prism::sdk {
namespace {
bool SameNativeTarget(const runtime::PopupSurfaceIdentity &a,
                      const runtime::PopupSurfaceIdentity &b)
{
    return a.worker == b.worker && a.target == b.target && a.lifetime == b.lifetime;
}

bool SameIntent(const runtime::PopupSurfaceRequest &a, const runtime::PopupSurfaceRequest &b)
{
    return a.scene == b.scene && a.popup_token == b.popup_token && a.active_node == b.active_node &&
           a.trigger == b.trigger &&
           a.parent_configure_generation == b.parent_configure_generation &&
           a.parent_window_geometry == b.parent_window_geometry && a.anchor == b.anchor &&
           a.desired_geometry == b.desired_geometry && a.gap == b.gap &&
           a.horizontal_alignment == b.horizontal_alignment &&
           a.vertical_preference == b.vertical_preference;
}

bool CurrentRequest(const runtime::Scene &scene, const runtime::PopupSurfaceRequest &request,
                    int configure_count)
{
    const auto input = scene.InputGeometry();
    return input && input->scene == request.scene && scene.PopupToken() == request.popup_token &&
           request.popup_token &&
           request.parent_configure_generation == static_cast<std::uint64_t>(configure_count);
}

bool ValidNativeIdentity(const runtime::PopupSurfaceIdentity &identity,
                         runtime::RenderWorkerGeneration worker)
{
    return identity.worker == worker.value && identity.target &&
           identity.target == identity.lifetime && identity.configure_generation;
}
} // namespace

void ClientApplication::Impl::ResetPopupSurface()
{
    if (scene && ui_popup_submitted_identity.submission_sequence) {
        scene->RevokePopupSurface(ui_popup_submitted_identity);
    }
    popup_configuration.reset();
    ui_popup_submitted_frame.reset();
    ui_popup_submitted_identity = {};
    rejected_popup_identity.reset();
    rejected_popup_request.reset();
}

void ClientApplication::Impl::RejectPopup(const runtime::PopupSurfaceIdentity &identity)
{
    if (rejected_popup_identity && *rejected_popup_identity == identity) {
        return;
    }
    runtime::RenderCommand command(runtime::RejectPopupCommand{installed_ui, identity});
    if (bridge->render_commands.TryPush(std::move(command)) != runtime::QueuePushResult::Accepted) {
        bridge->terminal.Fail(runtime::TerminalReason::CommandQueueFailure);
        throw std::runtime_error("Popup rejection command unavailable");
    }

    rejected_popup_identity = identity;
    if (popup_configuration) {
        rejected_popup_request = popup_configuration->request;
    }
    if (scene && ui_popup_submitted_identity.submission_sequence) {
        scene->RevokePopupSurface(ui_popup_submitted_identity);
    }
    ui_popup_submitted_frame.reset();
    ui_popup_submitted_identity = {};
}

void ClientApplication::Impl::HandlePopupConfigure(const runtime::PopupConfigureEvent &event)
{
    if (!scene || event.ui != installed_ui || !worker_generation || !event.request ||
        !ValidNativeIdentity(event.identity, *worker_generation) ||
        event.identity.configure_generation != event.configure.configure_generation ||
        event.configure.parent_configure_generation != event.request->parent_configure_generation ||
        !CurrentRequest(*scene, *event.request, ui_configure_count)) {
        return;
    }
    if (popup_configuration && SameNativeTarget(event.identity, popup_configuration->identity) &&
        event.identity.configure_generation <= popup_configuration->identity.configure_generation) {
        return;
    }

    if (ui_popup_submitted_identity.submission_sequence) {
        scene->RevokePopupSurface(ui_popup_submitted_identity);
    }
    ui_popup_submitted_frame.reset();
    ui_popup_submitted_identity = {};
    popup_configuration = event;
    rejected_popup_identity.reset();
    if (ui_metrics.scale != 1.0) {
        RejectPopup(event.identity);
    }
    InvalidateQueuedFrame();
    force_frame_capture = true;
    QueueRenderUpdate(true);
}

void ClientApplication::Impl::HandlePopupClosed(const runtime::PopupClosedEvent &event)
{
    if (!scene || event.ui != installed_ui || !worker_generation ||
        event.identity.worker != worker_generation->value) {
        return;
    }
    const bool configured =
        popup_configuration && SameNativeTarget(event.identity, popup_configuration->identity);
    const bool submitted =
        ui_popup_submitted_frame && SameNativeTarget(event.identity, ui_popup_submitted_identity);
    if (!configured && !submitted) {
        return;
    }

    if (submitted) {
        scene->RevokePopupSurface(ui_popup_submitted_identity);
        ui_popup_submitted_frame.reset();
        ui_popup_submitted_identity = {};
    }
    if (configured) {
        popup_configuration.reset();
    }
    if (event.reason == platform::WaylandPopupCloseReason::CompositorDismissed && event.request &&
        CurrentRequest(*scene, *event.request, ui_configure_count)) {
        scene->ClosePopup(runtime::PopupCloseReason::Unavailable);
    }
    InvalidateQueuedFrame();
    force_frame_capture = true;
    QueueRenderUpdate(true);
}

void ClientApplication::Impl::HandlePopupInput(const runtime::PopupInputEvent &event)
{
    if (!scene || event.ui != installed_ui || !worker_generation ||
        event.identity.worker != worker_generation->value) {
        return;
    }

    const auto input_ui = installed_ui;
    const auto result =
        scene->HandlePopupSurfaceInput(event.event, event.identity, event.input_snapshot);
    CompleteInteractionResult(result, input_ui);
}

void ClientApplication::Impl::HandlePopupSubmitted(const runtime::PopupSubmittedEvent &event)
{
    if (!scene || event.ui != installed_ui || !worker_generation || !event.frame ||
        event.frame->ui != event.ui || event.frame->worker != *worker_generation ||
        !event.sequence || event.sequence != event.frame->sequence || !event.frame->plan ||
        !ValidNativeIdentity(event.identity, *worker_generation) ||
        !SameNativeTarget(event.identity, event.frame->identity) ||
        event.identity.configure_generation != event.frame->identity.configure_generation ||
        !event.identity.submission_sequence || !popup_configuration ||
        !SameNativeTarget(event.identity, popup_configuration->identity) ||
        event.identity.configure_generation != popup_configuration->identity.configure_generation ||
        event.frame->plan->configure_generation != event.identity.configure_generation ||
        !CurrentRequest(*scene, event.frame->plan->request, ui_configure_count)) {
        return;
    }
    if (event.pixels && !event.pixel_submission) {
        return;
    }

    const bool first = !scene->HasPopupSurfaceAdoption();
    if (first && (!event.pixels || !event.pixel_submission)) {
        return;
    }
    if (!scene->AdoptPopupSurface(*event.frame->plan, event.identity)) {
        return;
    }

    ui_popup_submitted_frame = event.frame;
    ui_popup_submitted_identity = event.identity;
    if (first || runtime::Has(scene->PendingDirty(), runtime::Dirty::Layout) ||
        runtime::Has(scene->PendingDirty(), runtime::Dirty::Paint)) {
        InvalidateQueuedFrame();
        force_frame_capture = true;
        QueueRenderUpdate(true);
    }
    if (pending_animation_finish_sequence && event.sequence >= *pending_animation_finish_sequence) {
        SyncAnimationSampling();
    }
    if (ui_root_metadata_frame && ui_root_metadata_frame->ui == installed_ui &&
        ui_root_metadata_frame->configure_count == ui_configure_count &&
        ui_root_metadata_frame->scene_revision == scene->TransactionRevision() &&
        ui_root_metadata_frame->pixels_revision == scene->PixelsRevision() &&
        ui_root_metadata_frame->theme_generation == (theme ? theme->generation : 0) &&
        event.frame->plan->request.scene_revision == scene->TransactionRevision() &&
        event.frame->plan->request.pixels_revision == scene->PixelsRevision() &&
        !HasUnsubmittedPixels()) {
        scene->AcknowledgeComposite();
    }
}

void ClientApplication::Impl::CapturePopupFrame(runtime::FramePacket &packet)
{
    packet.popup_surface_request.reset();
    packet.popup_surface_frame.reset();
    packet.popup_surface_excluded = {};
    auto request = scene->CapturePopupSurfaceRequest(packet.configure_count);
    if (!request || packet.scale != 1.0) {
        if (scene->HasPopupSurfaceAdoption()) {
            scene->RevokePopupSurface(ui_popup_submitted_identity);
            ui_popup_submitted_frame.reset();
            ui_popup_submitted_identity = {};
        }
        return;
    }
    if (rejected_popup_request && SameIntent(*request, *rejected_popup_request) &&
        request->theme_generation == rejected_popup_request->theme_generation) {
        return;
    }
    rejected_popup_request.reset();
    if (scene->HasPopupSurfaceAdoption()) {
        packet.popup_surface_excluded = ui_popup_submitted_identity;
    }
    packet.popup_surface_request =
        std::make_shared<const runtime::PopupSurfaceRequest>(std::move(*request));
    if (!popup_configuration || !worker_generation ||
        !ValidNativeIdentity(popup_configuration->identity, *worker_generation) ||
        !popup_configuration->request ||
        !SameIntent(*packet.popup_surface_request, *popup_configuration->request)) {
        return;
    }

    std::string diagnostic;
    auto plan = scene->PreparePopupSurface(*packet.popup_surface_request,
                                           popup_configuration->configure, &diagnostic);
    if (!plan) {
        std::fprintf(stderr, "[prism-sdk] native popup fallback: %s\n", diagnostic.c_str());
        RejectPopup(popup_configuration->identity);
        packet.popup_surface_request.reset();
        return;
    }
    runtime::PopupFramePacket frame;
    frame.ui = packet.ui;
    frame.worker = *worker_generation;
    frame.identity = popup_configuration->identity;
    frame.identity.submission_sequence = 0;
    frame.sequence = packet.sequence;
    frame.resource_epoch = packet.resource_epoch;
    frame.image_uses = CollectImageUses(*plan->display_list);
    frame.plan = std::make_shared<const runtime::PopupSurfacePlan>(std::move(*plan));
    packet.popup_surface_frame =
        std::make_shared<const runtime::PopupFramePacket>(std::move(frame));
}
} // namespace prism::sdk
