#include "client_application_p.hpp"

#include <chrono>
#include <cstdio>

namespace prism::sdk {
namespace {

bool PushControl(runtime::PollableQueue<runtime::RenderCommand> &queue,
                 runtime::TerminalSignal &terminal, runtime::RenderCommand command) noexcept
{
    try {
        if (queue.TryPush(std::move(command)) == runtime::QueuePushResult::Accepted) {
            return true;
        }
    } catch (...) {
        // Failure is reported through the independent terminal signal.
    }

    terminal.Fail(runtime::TerminalReason::CommandQueueFailure);
    return false;
}

} // namespace

bool ClientApplication::Impl::OpenRenderWorker(std::string *failure, std::string *detail)
{
    if (render_owner || worker_generation || failed || closed) {
        if (failure) {
            *failure = "Render worker cannot be reopened";
        }
        return false;
    }

    const auto generation = bridge->worker_lifecycle.BeginOpen();
    if (!generation) {
        if (failure) {
            *failure = "Render worker open handshake unavailable";
        }
        return false;
    }
    worker_generation = *generation;

    try {
        render_owner = std::make_unique<ClientRenderOwner>(config, bridge->render_commands,
                                                           bridge->render_events, bridge->terminal,
                                                           bridge->worker_lifecycle);
        if (!render_owner->Start(*generation)) {
            bridge->worker_lifecycle.CompleteOpen(*generation,
                                                  runtime::RenderWorkerOpenOutcome::Failed);
            bridge->worker_lifecycle.CompleteClose(*generation);
            render_owner.reset();
            if (failure) {
                *failure = "Render worker thread unavailable";
            }
            return false;
        }
    } catch (const std::exception &error) {
        bridge->worker_lifecycle.CompleteOpen(*generation,
                                              runtime::RenderWorkerOpenOutcome::Failed);
        bridge->worker_lifecycle.CompleteClose(*generation);
        render_owner.reset();
        if (failure) {
            *failure = "Render worker creation failed";
        }
        if (detail) {
            *detail = error.what();
        }
        return false;
    }

    const auto result = bridge->worker_lifecycle.WaitOpen(*generation, std::chrono::seconds(12));
    if (result == runtime::RenderWorkerOpenWait::Opened &&
        bridge->terminal.Reason() == runtime::TerminalReason::None) {
        try {
            // The original Open sampled protocol capabilities synchronously.
            // Keep SDK getters ready before the first UI Pump, without consuming
            // the ordered configure events waiting in the reverse queue.
            ApplyRenderStatus(render_owner->ReadSnapshot());
            return true;
        } catch (const std::exception &error) {
            if (detail) {
                *detail = error.what();
            }
            if (failure) {
                *failure = "Render worker opening status unavailable";
            }
        }
    }

    if (failure && failure->empty()) {
        *failure = result == runtime::RenderWorkerOpenWait::TimedOut
                       ? "Render worker open timed out"
                       : "Wayland window open failed in render worker";
    }
    QueueStopRenderWorker();
    CloseRenderWorker();
    return false;
}

void ClientApplication::Impl::QueueStopRenderWorker() noexcept
{
    if (worker_generation) {
        bridge->worker_lifecycle.RequestClose(*worker_generation);
    } else {
        bridge->terminal.RequestStop();
    }
}

void ClientApplication::Impl::CloseRenderWorker() noexcept
{
    if (!render_owner) {
        return;
    }

    QueueStopRenderWorker();
    if (worker_generation) {
        const auto outcome =
            bridge->worker_lifecycle.WaitClose(*worker_generation, std::chrono::seconds(12));
        if (outcome != runtime::RenderWorkerCloseWait::Acknowledged) {
            std::fprintf(stderr, "[prism-sdk] render worker close acknowledgment timed out\n");
            bridge->terminal.Fail(runtime::TerminalReason::InternalFailure);
        }
    }

    // The bridge owns the queues, terminal and lifecycle. Join is mandatory
    // before that bridge can be replaced or destroyed, even after a timeout.
    render_owner->Join();
    try {
        ApplyRenderStatus(render_owner->ReadSnapshot());
    } catch (...) {
        bridge->terminal.Fail(runtime::TerminalReason::InternalFailure);
    }
    render_owner.reset();
}

bool ClientApplication::Impl::ResetRenderBridge() noexcept
{
    if (render_owner || opened_once || closed) {
        return false;
    }

    try {
        auto replacement = std::make_unique<ClientRenderBridge>();

        // A failed Open can leave preloaded CPU images alive. Replay their
        // exact versions into the new queue before the next InstallUi barrier.
        for (auto value : registered_images) {
            const contracts::ResourceId id{value};
            const auto generation = resources.Generation(id);
            auto pixels = resources.Retain(id);
            if (!generation || !pixels) {
                throw std::runtime_error("Retained image unavailable after failed Open");
            }

            runtime::RenderCommand registration(
                runtime::RegisterImageCommand{{id, generation}, std::move(pixels)});
            if (replacement->render_commands.TryPush(std::move(registration)) !=
                runtime::QueuePushResult::Accepted) {
                throw std::runtime_error("Image replay queue unavailable after failed Open");
            }
        }

        bridge.swap(replacement);
        worker_generation.reset();
        last_processed_window_sequence = 0;
        uploaded_image_versions.clear();
        pending_release_versions.clear();
        queued_frame.reset();
        ui_submitted_frame.reset();
        animation_worker_active = false;
        animation_deadline_ns.reset();
        pending_animation_finish_sequence.reset();
        platform_status = {};
        ui_metrics = {};
        ui_configure_count = 0;
        return true;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[prism-sdk] render bridge reset failed: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "[prism-sdk] render bridge reset failed\n");
    }

    bridge->terminal.Fail(runtime::TerminalReason::InternalFailure);
    failed = true;
    return false;
}

void ClientApplication::Impl::QueueRenderInstallUi(runtime::UiLoadId load)
{
    if (!load.owner || !load.generation ||
        !PushControl(bridge->render_commands, bridge->terminal,
                     runtime::RenderCommand(runtime::InstallUiCommand{load}))) {
        throw std::runtime_error("Render install barrier unavailable");
    }
}

void ClientApplication::Impl::QueueRenderInvalidate(runtime::UiLoadId load)
{
    PushControl(bridge->render_commands, bridge->terminal,
                runtime::RenderCommand(runtime::InvalidateFrameCommand{load}));
}

void ClientApplication::Impl::InvalidateQueuedFrame()
{
    if (!queued_frame) {
        return;
    }

    QueueRenderInvalidate(installed_ui);
    queued_frame.reset();
}

void ClientApplication::Impl::QueueRenderUpdate(bool deferred)
{
    PushControl(bridge->render_commands, bridge->terminal,
                runtime::RenderCommand(
                    runtime::RequestRenderCommand{runtime::RenderRequestKind::Update, deferred}));
}

void ClientApplication::Impl::QueueRenderRedraw(bool deferred)
{
    PushControl(bridge->render_commands, bridge->terminal,
                runtime::RenderCommand(
                    runtime::RequestRenderCommand{runtime::RenderRequestKind::Redraw, deferred}));
}

} // namespace prism::sdk
