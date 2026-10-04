#include "client_render_owner_p.hpp"

#include <array>
#include <cstdio>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>

namespace prism::sdk {
namespace {
bool SameSignificantStatus(const runtime::RenderStatusEvent &left,
                           const runtime::RenderStatusEvent &right) noexcept
{
    const auto &a = left.platform;
    const auto &b = right.platform;
    const bool platform =
        a.close_requested == b.close_requested && a.configured == b.configured &&
        a.mapped == b.mapped && a.frame_callback_pending == b.frame_callback_pending &&
        a.presentation_feedback == b.presentation_feedback &&
        a.metrics.logical_size == b.metrics.logical_size &&
        a.metrics.buffer_size.width == b.metrics.buffer_size.width &&
        a.metrics.buffer_size.height == b.metrics.buffer_size.height &&
        a.metrics.scale == b.metrics.scale && a.configure_count == b.configure_count &&
        a.frame_done_count == b.frame_done_count && a.presentation_count == b.presentation_count &&
        a.surface_state_commits == b.surface_state_commits &&
        a.surface_pixel_commits == b.surface_pixel_commits &&
        a.surface_submission_failures == b.surface_submission_failures &&
        a.surface_noops == b.surface_noops;

    const auto &c = left.backend;
    const auto &d = right.backend;
    const bool backend =
        c.gpu_render_attempts == d.gpu_render_attempts &&
        c.gpu_render_successes == d.gpu_render_successes && c.swap_attempts == d.swap_attempts &&
        c.swap_successes == d.swap_successes && c.full_pixel_repairs == d.full_pixel_repairs &&
        c.partial_pixel_repairs == d.partial_pixel_repairs &&
        c.empty_pixel_repairs == d.empty_pixel_repairs &&
        c.pixel_repair_pixels == d.pixel_repair_pixels &&
        c.content_damage_pixels == d.content_damage_pixels &&
        c.damage_history_commits == d.damage_history_commits &&
        c.buffer_age_queries == d.buffer_age_queries &&
        c.unknown_buffer_ages == d.unknown_buffer_ages && c.last_buffer_age == d.last_buffer_age &&
        c.buffer_age_supported == d.buffer_age_supported &&
        c.swap_damage_supported == d.swap_damage_supported &&
        c.partial_update_supported == d.partial_update_supported &&
        c.image_uploads == d.image_uploads && c.upload_bytes == d.upload_bytes &&
        c.oversized_uploads == d.oversized_uploads;

    const auto &e = left.startup;
    const auto &f = right.startup;
    const bool startup = e.egl_init_us == f.egl_init_us && e.ganesh_init_us == f.ganesh_init_us &&
                         e.first_render_us == f.first_render_us &&
                         e.first_swap_us == f.first_swap_us;

    // Wait duration alone is diagnostic, not a reason to put a status barrier
    // between consecutive pointer motions or to schedule a stable frame.
    return platform && backend && startup && left.gl_renderer == right.gl_renderer &&
           left.presented == right.presented;
}
} // namespace

ClientRenderOwner::ClientRenderOwner(ClientConfig config,
                                     runtime::PollableQueue<runtime::RenderCommand> &commands,
                                     runtime::PollableQueue<runtime::RenderEvent> &events,
                                     runtime::TerminalSignal &terminal,
                                     runtime::RenderWorkerLifecycle &lifecycle)
    : config_(std::move(config)), commands_(commands), events_(events), terminal_(terminal),
      lifecycle_(lifecycle)
{
}

ClientRenderOwner::~ClientRenderOwner()
{
    Join();
}

bool ClientRenderOwner::Start(runtime::RenderWorkerGeneration generation)
{
    if (worker_.joinable() ||
        lifecycle_.OpenStatus(generation) != runtime::RenderWorkerOpenStatus::Started) {
        return false;
    }

    try {
        worker_ = std::thread(&ClientRenderOwner::Run, this, generation);
        return true;
    } catch (const std::system_error &) {
        lifecycle_.CompleteOpen(generation, runtime::RenderWorkerOpenOutcome::Failed);
        lifecycle_.CompleteClose(generation);
        return false;
    }
}

void ClientRenderOwner::Join()
{
    if (worker_.joinable()) {
        worker_.join();
    }
}

ClientRenderOwner::Snapshot ClientRenderOwner::ReadSnapshot() const
{
    std::lock_guard lock(snapshot_mutex_);
    return snapshot_;
}

bool ClientRenderOwner::OpenWindow()
{
    window_.DeferCloseRequests();
    window_.SetEventHandler(std::bind_front(&ClientRenderOwner::QueueWindowEvent, this));
    window_.SetSubmitHandlers(std::bind_front(&ClientRenderOwner::PrepareSubmit, this),
                              std::bind_front(&ClientRenderOwner::CommitPixels, this),
                              std::bind_front(&ClientRenderOwner::Submitted, this));
    window_.SetPresentationHandler(
        std::bind_front(&ClientRenderOwner::QueuePresentationEvent, this));

    const platform::WaylandOpenOptions options{terminal_.Fd(), std::chrono::steady_clock::now() +
                                                                   std::chrono::seconds(10)};
    return window_.Open(config_.socket, config_.app_id, config_.title, config_.width,
                        config_.height, options);
}

void ClientRenderOwner::Run(runtime::RenderWorkerGeneration generation) noexcept
{
    bool open_reported = false;
    worker_generation_ = generation;
    try {
        damage_commands_ = std::make_unique<render_skia::RasterRenderer>(config_.font_path);
        if (!damage_commands_->Ready()) {
            throw std::runtime_error("Render damage font unavailable");
        }

        // Install the already queued UI identity before Open's Wayland
        // roundtrips can deliver input. Image registration only prepares CPU
        // resources here; uploads still wait for a configured surface.
        if (!DrainCommands()) {
            throw std::runtime_error("Initial render commands failed");
        }

        const bool opened = OpenWindow();
        PublishStatus();
        const auto outcome = terminal_.StopRequested() ? runtime::RenderWorkerOpenOutcome::Cancelled
                             : opened && terminal_.Reason() == runtime::TerminalReason::None
                                 ? runtime::RenderWorkerOpenOutcome::Opened
                                 : runtime::RenderWorkerOpenOutcome::Failed;
        open_reported = lifecycle_.CompleteOpen(generation, outcome);
        if (outcome == runtime::RenderWorkerOpenOutcome::Opened && open_reported) {
            while (!terminal_.StopRequested() &&
                   terminal_.Reason() == runtime::TerminalReason::None) {
                if (!DrainCommands() || !AdvanceImageUploads()) {
                    terminal_.Fail(runtime::TerminalReason::RenderFailure);
                    break;
                }
                PublishStatus();
                if (window_.IsCloseRequested() || terminal_.StopRequested() ||
                    terminal_.Reason() != runtime::TerminalReason::None) {
                    break;
                }

                std::array<pollfd, 2> wake{
                    {{commands_.Fd(), POLLIN, 0}, {terminal_.Fd(), POLLIN, 0}}};
                const int timeout = !upload_queue_.empty() && window_.IsConfigured() ? 0 : -1;
                const bool running = window_.Pump(timeout, wake);
                PublishStatus();
                if (!running) {
                    if (!window_.IsCloseRequested() && !terminal_.StopRequested() &&
                        terminal_.Reason() == runtime::TerminalReason::None) {
                        terminal_.Fail(runtime::TerminalReason::PlatformFailure);
                    }
                    break;
                }
            }
        }
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[prism-sdk] render worker failed: %s\n", error.what());
        terminal_.Fail(runtime::TerminalReason::RenderFailure);
    } catch (...) {
        terminal_.Fail(runtime::TerminalReason::RenderFailure);
    }

    if (!open_reported) {
        const auto outcome = terminal_.StopRequested() ? runtime::RenderWorkerOpenOutcome::Cancelled
                                                       : runtime::RenderWorkerOpenOutcome::Failed;
        lifecycle_.CompleteOpen(generation, outcome);
    }

    CloseRenderState();
    try {
        PublishStatus(false);
    } catch (...) {
        terminal_.Fail(runtime::TerminalReason::InternalFailure);
    }
    lifecycle_.CompleteClose(generation);
}

void ClientRenderOwner::CloseGpu() noexcept
{
    animation_sampling_active_ = false;
    ResetFrameOpportunity();
    damage_history_.Invalidate();
    prepared_damage_.reset();
    prepared_frame_.reset();
    render_frame_.reset();
    input_snapshot_.reset();
    input_ui_ = {};
    committed_frame_.reset();
    committed_damage_resource_epoch_ = 0;

    if (renderer_) {
        if (!egl_.MakeCurrent()) {
            renderer_->Abandon();
        }
        renderer_.reset();
    }
    egl_.Close();
    render_images_.clear();
    upload_queue_.clear();
    queued_uploads_.clear();
}

void ClientRenderOwner::CloseRenderState() noexcept
{
    window_.DeferCloseRequests();
    window_.SetEventHandler({});
    window_.SetSubmitHandlers({}, {}, {});
    window_.SetPresentationHandler({});
    CloseGpu();
    window_.Close();
    damage_commands_.reset();
}

bool ClientRenderOwner::QueueEvent(runtime::RenderEvent event) noexcept
{
    try {
        if (events_.TryPush(std::move(event)) == runtime::QueuePushResult::Accepted) {
            return true;
        }
    } catch (...) {
        // A Wayland C listener may be on this stack.
    }

    terminal_.Fail(runtime::TerminalReason::EventQueueFailure);
    return false;
}

void ClientRenderOwner::QueueWindowEvent(const contracts::WindowEvent &event) noexcept
{
    if (issued_event_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        terminal_.Fail(runtime::TerminalReason::InternalFailure);
        return;
    }

    try {
        const auto sequence = issued_event_sequence_ + 1;
        runtime::RenderEvent copy(
            runtime::SequencedWindowEvent{event, sequence, input_ui_, input_snapshot_});
        const auto result =
            events_.TryPushLatest(std::move(copy), runtime::ReplacePointerMotionTail);
        if (result == runtime::QueuePushResult::Accepted ||
            result == runtime::QueuePushResult::Replaced) {
            issued_event_sequence_ = sequence;
            return;
        }
    } catch (...) {
        // A Wayland C listener may be on this stack.
    }

    terminal_.Fail(runtime::TerminalReason::EventQueueFailure);
}

void ClientRenderOwner::QueuePresentationEvent(const platform::PixelPresentation &event) noexcept
{
    try {
        QueueEvent(runtime::RenderEvent(event));
    } catch (...) {
        terminal_.Fail(runtime::TerminalReason::EventQueueFailure);
    }
}

void ClientRenderOwner::PublishStatus(bool enqueue)
{
    const auto submitted = window_.GetSubmitStats();
    Snapshot status;
    status.platform = {
        .close_requested = window_.IsCloseRequested(),
        .configured = window_.IsConfigured(),
        .mapped = window_.IsMapped(),
        .frame_callback_pending = window_.FrameCallbackPending(),
        .presentation_feedback = window_.HasPresentationFeedback(),
        .metrics = window_.IsConfigured() ? window_.Metrics() : contracts::WindowMetrics{},
        .configure_count = window_.ConfigureCount(),
        .frame_done_count = window_.FrameDoneCount(),
        .presentation_count = window_.PresentationCount(),
        .wait_duration_ns = window_.WaitDurationNs(),
        .surface_state_commits = submitted.state_commits,
        .surface_pixel_commits = submitted.pixel_commits,
        .surface_submission_failures = submitted.failures,
        .surface_noops = submitted.none,
    };
    status.backend = backend_stats_;
    status.startup = startup_stats_;
    status.gl_renderer = gl_renderer_;
    status.presented = presented_;

    {
        std::lock_guard lock(snapshot_mutex_);
        snapshot_ = status;
    }

    if (!enqueue) {
        return;
    }

    if (last_published_status_ && SameSignificantStatus(*last_published_status_, status)) {
        return;
    }

    last_published_status_ = status;
    runtime::RenderEvent event(std::move(status));
    const auto result = events_.TryPushLatest(std::move(event), runtime::ReplaceStatusTail);
    if (result != runtime::QueuePushResult::Accepted &&
        result != runtime::QueuePushResult::Replaced) {
        terminal_.Fail(runtime::TerminalReason::EventQueueFailure);
        return;
    }
}

} // namespace prism::sdk
