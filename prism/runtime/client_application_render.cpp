#include "client_application_p.hpp"

namespace prism::sdk {

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
        if (const auto *ready = std::get_if<runtime::FrameOpportunityEvent>(&*event)) {
            // A configure can retire an unanswered opportunity and issue a
            // replacement before the UI drains the reverse queue. Keep the
            // newest opportunity and validate its generation after the batch.
            opportunity = *ready;
            continue;
        }

        const auto &window = std::get<runtime::SequencedWindowEvent>(*event);
        if (!scene || !window.sequence || window.sequence <= last_processed_window_sequence) {
            bridge->terminal.Fail(runtime::TerminalReason::EventQueueFailure);
            FailFrontend();
            break;
        }
        last_processed_window_sequence = window.sequence;
        processed_window_sequence = window.sequence;

        // A prior action in this ordered batch may have changed hit bounds.
        if (runtime::Has(scene->PendingDirty(), runtime::Dirty::Layout)) {
            PublishFramePacket();
        }
        const bool configured = std::holds_alternative<contracts::ConfigureEvent>(window.event);
        HandleWindowEvent(window.event);
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

void ClientApplication::Impl::HandleWindowEvent(const contracts::WindowEvent &event)
{
    if (auto *configure = std::get_if<contracts::ConfigureEvent>(&event)) {
        if (configure->configure_count <= ui_configure_count) {
            throw std::runtime_error("Out-of-order Wayland configure event");
        }

        ui_metrics = configure->metrics;
        ui_configure_count = configure->configure_count;
        scene->SetViewport(configure->metrics.logical_size);
        queued_frame.reset();
    } else if (auto *motion = std::get_if<contracts::PointerMotionEvent>(&event)) {
        if (scene->SetPointer(motion->position)) {
            queued_frame.reset();
            QueueRenderUpdate(true);
        }
    } else if (auto *key = std::get_if<contracts::KeyEvent>(&event)) {
        if (key->state == contracts::ButtonState::Pressed) {
            if (key->physical_key == 0x2B && scene->FocusNext()) {
                queued_frame.reset();
                QueueRenderUpdate(true);
            } else if ((key->physical_key == 0x28 || key->physical_key == 0x2C) && on_action) {
                if (auto action = scene->FocusedAction()) {
                    on_action(*action);
                }
            }
        }
    } else if (auto *button = std::get_if<contracts::PointerButtonEvent>(&event)) {
        if (button->state == contracts::ButtonState::Pressed &&
            button->button == contracts::PointerButton::Primary && on_action) {
            if (auto action = scene->ActionAt(button->position)) {
                on_action(*action);
            }
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

    // A later Scene, theme or pixel update must not be acknowledged by an
    // older State/None/Pixels result waiting in the reverse queue.
    if (event.metadata_prepared && scene && event.ui == installed_ui &&
        event.scene_revision == scene->TransactionRevision() &&
        event.pixels_revision == scene->PixelsRevision() &&
        event.theme_generation == (theme ? theme->generation : 0)) {
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
