#include "client_application_p.hpp"

namespace prism::sdk {
namespace {
std::uint64_t OrderedInputSequence(const runtime::RenderEvent &event)
{
    if (const auto *popup = std::get_if<runtime::PopupConfigureEvent>(&event)) {
        return popup->sequence;
    }
    if (const auto *popup = std::get_if<runtime::PopupClosedEvent>(&event)) {
        return popup->sequence;
    }
    if (const auto *popup = std::get_if<runtime::PopupInputEvent>(&event)) {
        return popup->sequence;
    }
    return std::get<runtime::SequencedWindowEvent>(event).sequence;
}
} // namespace

void ClientApplication::Impl::ProcessRenderEvents(bool deliver)
{
    std::uint64_t processed_window_sequence{};
    std::optional<runtime::FrameOpportunityEvent> opportunity;
    while (auto event = bridge->render_events.TryPop()) {
        if (!deliver || failed || closed ||
            bridge->terminal.Reason() != runtime::TerminalReason::None) {
            continue;
        }

        if (const auto *status = std::get_if<runtime::RenderStatusEvent>(&*event)) {
            ApplyRenderStatus(*status);
            continue;
        }
        if (close_accept_queued) {
            // A queued acceptance has already revoked frontend input. Continue
            // observing worker status, but never edit a document after its
            // final save/close decision from another event in this batch.
            continue;
        }
        if (const auto *uploaded = std::get_if<runtime::ImageUploadedEvent>(&*event)) {
            const auto version = uploaded->version;
            if (resources.Generation(version.id) == version.generation &&
                registered_images.contains(version.id.value)) {
                uploaded_image_versions.insert_or_assign(version.id.value, version.generation);
            }
            continue;
        }
        if (const auto *released = std::get_if<runtime::ImageReleasedEvent>(&*event)) {
            pending_release_versions.erase(
                {released->version.id.value, released->version.generation});
            continue;
        }
        if (const auto *submitted = std::get_if<runtime::SubmittedFrameEvent>(&*event)) {
            HandleSubmitted(*submitted);
            continue;
        }
        if (const auto *presentation = std::get_if<platform::PixelPresentation>(&*event)) {
            HandlePresentation(*presentation);
            continue;
        }
        if (const auto *submitted = std::get_if<runtime::PopupSubmittedEvent>(&*event)) {
            HandlePopupSubmitted(*submitted);
            continue;
        }
        if (std::holds_alternative<runtime::PopupPresentationEvent>(*event)) {
            // Child feedback belongs to its native target. It cannot satisfy
            // the root UI/Preview/Master presentation milestone.
            continue;
        }
        if (const auto *ready = std::get_if<runtime::FrameOpportunityEvent>(&*event)) {
            // A configure can retire an unanswered opportunity and issue a
            // replacement before the UI drains the reverse queue. Keep the
            // newest opportunity and validate its generation after the batch.
            opportunity = *ready;
            continue;
        }

        const auto sequence = OrderedInputSequence(*event);
        if (!scene || !sequence || sequence <= last_processed_window_sequence) {
            bridge->terminal.Fail(runtime::TerminalReason::EventQueueFailure);
            FailFrontend();
            break;
        }
        last_processed_window_sequence = sequence;
        processed_window_sequence = sequence;

        if (const auto *configured = std::get_if<runtime::PopupConfigureEvent>(&*event)) {
            HandlePopupConfigure(*configured);
            continue;
        }
        if (const auto *closed_popup = std::get_if<runtime::PopupClosedEvent>(&*event)) {
            HandlePopupClosed(*closed_popup);
            continue;
        }
        if (const auto *input = std::get_if<runtime::PopupInputEvent>(&*event)) {
            if (runtime::Has(scene->PendingDirty(), runtime::Dirty::Layout)) {
                PublishFramePacket();
            }
            HandlePopupInput(*input);
            continue;
        }

        const auto &window = std::get<runtime::SequencedWindowEvent>(*event);

        // Input belongs to the UI load observed by the protocol owner. A
        // queued press from a replaced load must never target the new Scene.
        // Configure and Close describe the persistent surface, not a UI load.
        const bool configured = std::holds_alternative<contracts::ConfigureEvent>(window.event);
        const bool closing = std::holds_alternative<contracts::CloseRequestedEvent>(window.event);
        if (!configured && !closing && window.ui != installed_ui) {
            continue;
        }

        // A prior action in this ordered batch may have changed hit bounds.
        if (runtime::Has(scene->PendingDirty(), runtime::Dirty::Layout)) {
            PublishFramePacket();
        }
        HandleWindowEvent(window.event, window.input_snapshot);
        if (configured) {
            PublishFramePacket();
        }
    }

    if (!scene || failed || closed || bridge->terminal.Reason() != runtime::TerminalReason::None) {
        return;
    }

    if (opportunity) {
        HandleFrameOpportunity(*opportunity);
    }
    if (!processed_window_sequence) {
        return;
    }

    // A window event is not consumed by the render owner merely because its
    // reverse queue became empty. Publish the resulting frame before acking
    // the highest sequence so old pixels cannot race UI input processing.
    PublishFramePacket();
    runtime::RenderCommand acknowledgment(
        runtime::UiEventsProcessedCommand{processed_window_sequence});
    if (bridge->render_commands.TryPush(std::move(acknowledgment)) !=
        runtime::QueuePushResult::Accepted) {
        bridge->terminal.Fail(runtime::TerminalReason::CommandQueueFailure);
        throw std::runtime_error("Render input acknowledgment unavailable");
    }
}

void ClientApplication::Impl::HandleWindowEvent(
    const contracts::WindowEvent &event, const std::shared_ptr<const runtime::InputSnapshot> &input)
{
    if (auto *configure = std::get_if<contracts::ConfigureEvent>(&event)) {
        if (configure->configure_count <= ui_configure_count) {
            throw std::runtime_error("Out-of-order Wayland configure event");
        }

        ResetPopupSurface();
        ui_metrics = configure->metrics;
        ui_configure_count = configure->configure_count;
        scene->SetViewport(configure->metrics.logical_size);
        ReconcileOwnerTask();
        UpdateOwnerConfirmationText();
        UpdateOwnerFilePanelText();
        UpdateOwnerFeedbackText();
        queued_frame.reset();
        return;
    }

    if (std::holds_alternative<contracts::CloseRequestedEvent>(event)) {
        if (!close_accept_queued && (!on_close_requested || on_close_requested())) {
            AcceptClose();
        }
        return;
    }
    ObserveOwnerFeedbackEvent(event);
    const auto input_ui = installed_ui;
    const bool popup_key =
        std::holds_alternative<contracts::KeyEvent>(event) && scene->HasPopupSurfaceAdoption() &&
        ui_popup_submitted_frame && ui_popup_submitted_frame->ui == installed_ui &&
        ui_popup_submitted_frame->plan && input &&
        input->scene == ui_popup_submitted_frame->plan->request.scene &&
        input->owner_modal_epoch == scene->OwnerModalEpoch() &&
        ui_popup_submitted_frame->plan->input_snapshot &&
        ui_popup_submitted_frame->plan->input_snapshot->owner_modal_epoch ==
            scene->OwnerModalEpoch() &&
        input->popup_token == scene->PopupToken() &&
        ui_popup_submitted_frame->plan->request.popup_token == scene->PopupToken();
    const auto result =
        popup_key ? scene->HandlePopupSurfaceInput(event, ui_popup_submitted_identity,
                                                   ui_popup_submitted_frame->plan->input_snapshot)
                  : scene->HandleInput(event, input);
    CompleteInteractionResult(result, input_ui);
}

void ClientApplication::Impl::CompleteInteractionResult(const runtime::InteractionResult &result,
                                                        runtime::UiLoadId input_ui)
{
    ReconcileOwnerTask();
    ReconcileOwnerFeedback();
    ReconcileTooltip();
    if (result.changed) {
        queued_frame.reset();
        QueueRenderUpdate(true);
    }
    if (result.changed && scene->HasActiveAnimations()) {
        // Input state changes can start a timeline without a business binding.
        // Stopping waits until PublishFramePacket retains the final pixels.
        SyncAnimationSampling();
    }
    if (result.control_edit) {
        control_delivery.Enqueue(input_ui, *result.control_edit);
    }
    DeliverInteractionEvents();
    if (result.text_edit && on_text_edit && input_ui == installed_ui && !closed && !failed) {
        on_text_edit(result.text_edit->action, result.text_edit->text);
    }
    if (result.activation && input_ui == installed_ui && !closed && !failed) {
        // The Scene has finished the input sequence before business code may
        // replace a region, install a new UI, or close this application.
        if (!HandleOwnerFeedbackAction(*result.activation) && on_action) {
            on_action(result.activation->action);
        }
    }
}

void ClientApplication::Impl::HandleSubmitted(const runtime::SubmittedFrameEvent &event)
{
    if (event.metadata_prepared && (!event.frame || event.frame->sequence != event.frame_sequence ||
                                    event.frame->ui != event.ui)) {
        bridge->terminal.Fail(runtime::TerminalReason::EventQueueFailure);
        FailFrontend();
        return;
    }
    if (event.kind == runtime::SubmittedKind::Pixels) {
        if (!event.metadata_prepared || !event.submission) {
            bridge->terminal.Fail(runtime::TerminalReason::EventQueueFailure);
            FailFrontend();
            return;
        }

        // A fast sequence of UI installations can evict an older load from
        // the two-slot presentation tracker before its ordered swap result is
        // consumed. That retired result is valid, but cannot prove that the
        // current UI was submitted or presented.
        const bool retired = event.ui.owner == installed_ui.owner && event.ui.generation > 0 &&
                             event.ui.generation < installed_ui.generation &&
                             !ui_presentation.Get(event.ui).installed;
        if (retired) {
            return;
        }

        if (!ui_presentation.Submit(event.ui, event.submission, event.feedback_expected)) {
            FailFrontend();
            return;
        }
        ui_submitted_frame = event.frame;
    }

    if (event.metadata_prepared && scene && event.ui == installed_ui) {
        const bool changed = scene->ApplyInputSnapshot(event.frame->input_snapshot);
        // Adoption may be valid without changing pixels. The task readiness
        // gate checks the accepted identity rather than the visual-change result.
        AdoptOwnerTaskInput(event.frame->input_snapshot, event.ui);
        if (changed) {
            // A stationary pointer is rehit only after the worker adopted geometry.
            InvalidateQueuedFrame();
            QueueRenderUpdate(true);
            if (scene->HasActiveAnimations()) {
                SyncAnimationSampling();
            }
        }
    }

    if (event.metadata_prepared && event.ui == installed_ui) {
        ui_root_metadata_frame = event.frame;
        AdoptOwnerFeedbackInput(event.frame->input_snapshot, event.ui);
        ReconcileTooltip();
    }

    // A later Scene, theme or pixel update must not be acknowledged by an
    // older State/None/Pixels result waiting in the reverse queue.
    if (event.metadata_prepared && scene && event.ui == installed_ui &&
        event.scene_revision == scene->TransactionRevision() &&
        event.pixels_revision == scene->PixelsRevision() &&
        event.theme_generation == (theme ? theme->generation : 0) && !HasUnsubmittedPopupPixels()) {
        scene->AcknowledgeComposite();
    }
    if (event.kind == runtime::SubmittedKind::Pixels && on_ui_submitted) {
        on_ui_submitted(event.ui);
    }
    if (event.kind == runtime::SubmittedKind::Pixels && event.ui == installed_ui &&
        pending_animation_finish_sequence &&
        event.frame_sequence >= *pending_animation_finish_sequence) {
        SyncAnimationSampling();
    }
}

void ClientApplication::Impl::HandlePresentation(const platform::PixelPresentation &event)
{
    const auto load = ui_presentation.Present(event);
    if (load != installed_ui || event.outcome != platform::PresentationOutcome::Discarded) {
        return;
    }
    const auto state = ui_presentation.Get(load);
    if (state.installed && !state.presented && event.submission.value == state.last_submission) {
        queued_frame.reset();
        force_frame_capture = true;
        PublishFramePacket();
        QueueRenderRedraw(true);
    }
}

} // namespace prism::sdk
