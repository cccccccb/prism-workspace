#include "presentation-time-client-protocol.h"
#include "prism-surface-effects-client.h"
#include "prism/contracts/rounded_region.hpp"
#include "wayland_window_p.hpp"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

namespace prism::platform {

WaylandWindow::WaylandWindow() = default;

WaylandWindow::~WaylandWindow()
{
    // Explicit Close delivers cancellation while the owner is alive. Object
    // teardown must not invoke callbacks whose bound owner may be gone.
    event_handler_ = {};
    destructing_ = true;
    Close();
}

void WaylandWindow::Emit(contracts::WindowEvent event)
{
    if (deferred_close_) {
        return;
    }
    try {
        if (event_handler_) {
            event_handler_(event);
        }
    } catch (...) {
        failed_ = true;
        Close();
    }
}

WaylandWindow::ShmBuffer *WaylandWindow::AcquireBuffer()
{
    const int width = static_cast<int>(metrics_.buffer_size.width);
    const int height = static_cast<int>(metrics_.buffer_size.height);
    for (auto &buffer : buffers_) {
        if (!buffer->busy && buffer->width == width && buffer->height == height) {
            return buffer.get();
        }
    }
    if (buffers_.size() >= 3) {
        return nullptr;
    }
    const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;
    int fd = memfd_create("prism-wayland-probe", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        return nullptr;
    }
    void *pixels = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    wl_shm_pool *pool = wl_shm_create_pool(shm_, fd, static_cast<int>(bytes));
    wl_buffer *handle =
        pool ? wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888)
             : nullptr;
    if (pool) {
        wl_shm_pool_destroy(pool);
    }
    close(fd);
    if (!handle) {
        munmap(pixels, bytes);
        return nullptr;
    }
    auto buffer = std::make_unique<ShmBuffer>();
    buffer->handle = handle;
    buffer->pixels = pixels;
    buffer->bytes = bytes;
    buffer->width = width;
    buffer->height = height;
    static const wl_buffer_listener listener{.release = BufferRelease};
    wl_buffer_add_listener(handle, &listener, buffer.get());
    buffers_.push_back(std::move(buffer));
    return buffers_.back().get();
}

void WaylandWindow::ReapBuffers()
{
    const int width = static_cast<int>(metrics_.buffer_size.width);
    const int height = static_cast<int>(metrics_.buffer_size.height);
    std::erase_if(buffers_, [=](const auto &buffer) {
        return !buffer->busy && (buffer->width != width || buffer->height != height);
    });
}

bool WaylandWindow::PresentationCapacityAvailable() const noexcept
{
    if (!presentation_) {
        return true;
    }
    for (const auto &pending : feedbacks_) {
        if (!pending.submission) {
            return true;
        }
    }
    return false;
}

bool WaylandWindow::PresentationPending(PixelSubmissionId submission) const noexcept
{
    if (!submission) {
        return false;
    }
    for (const auto &pending : feedbacks_) {
        if (pending.submission == submission && pending.committed) {
            return true;
        }
    }
    return false;
}

struct wp_presentation_feedback *WaylandWindow::TrackPresentation(PixelSubmissionId submission)
{
    if (!presentation_) {
        return nullptr;
    }
    for (auto &pending : feedbacks_) {
        if (pending.submission) {
            continue;
        }
        auto *feedback = wp_presentation_feedback(presentation_, surface_);
        if (!feedback) {
            return nullptr;
        }
        static const wp_presentation_feedback_listener listener{.sync_output = PresentationOutput,
                                                                .presented = PresentationDone,
                                                                .discarded = PresentationDiscarded};
        if (wp_presentation_feedback_add_listener(feedback, &listener, this) < 0) {
            wp_presentation_feedback_destroy(feedback);
            return nullptr;
        }
        pending = {feedback, submission, false, {}};
        return feedback;
    }
    return nullptr;
}

void WaylandWindow::ConfirmPixelSubmission(PixelSubmissionId submission) noexcept
{
    last_pixel_submission_ = submission;
    for (auto &pending : feedbacks_) {
        if (pending.submission == submission) {
            pending.committed = true;
            return;
        }
    }
}

SubmitResult WaylandWindow::CompleteSubmit(SubmitResult result)
{
    switch (result) {
    case SubmitResult::None:
        ++submit_stats_.none;
        break;
    case SubmitResult::State:
        ++submit_stats_.state_commits;
        break;
    case SubmitResult::Pixels:
        ++submit_stats_.pixel_commits;
        break;
    case SubmitResult::Failed:
        ++submit_stats_.failures;
        failed_ = true;
        break;
    case SubmitResult::Deferred:
    case SubmitResult::AwaitFrame:
        return result;
    }
    if (on_submitted_) {
        on_submitted_(result);
    }
    if (result == SubmitResult::Pixels) {
        DispatchCompletedPresentations();
    }
    // Destroying a client callback proxy cannot undo its pending server-side
    // frame request. Terminal failure destroys the surface, never commits it.
    if (result == SubmitResult::Failed && !dispatching_) {
        Close();
    }
    return result;
}

SubmitResult WaylandWindow::TrySubmit()
{
    if (failed_) {
        return SubmitResult::Failed;
    }
    if (closing_ || deferred_close_ || !configured_ || !update_requested_ || !surface_) {
        return SubmitResult::None;
    }
    if (submission_deferred_) {
        return SubmitResult::Deferred;
    }
    const bool allow_pixels = frame_callback_ == nullptr && PresentationCapacityAvailable();
    if (prepare_submit_) {
        const SubmitRequest request{display_,
                                    surface_,
                                    static_cast<int>(metrics_.buffer_size.width),
                                    static_cast<int>(metrics_.buffer_size.height),
                                    allow_pixels,
                                    force_pixels_};
        auto result = prepare_submit_(request);
        if (result == SubmitResult::Failed) {
            return CompleteSubmit(result);
        }
        if (result == SubmitResult::Deferred) {
            // A configure/frame listener and Pump may both reach TrySubmit in
            // one turn. A bounded preparation defers at most once per turn.
            submission_deferred_ = true;
            return result;
        }
        if (result == SubmitResult::AwaitFrame) {
            // The UI has not published a compatible frame. A configure ACK or
            // other State-only change can progress without consuming the
            // forced pixel request or requesting a pixel frame callback.
            if (state_pending_) {
                wl_surface_commit(surface_);
                state_pending_ = false;
                return CompleteSubmit(SubmitResult::State);
            }
            return result;
        }
        if (force_pixels_ && allow_pixels && result != SubmitResult::Pixels) {
            return CompleteSubmit(SubmitResult::Failed);
        }
        if (result == SubmitResult::Pixels) {
            if (!allow_pixels || !commit_pixels_) {
                return CompleteSubmit(SubmitResult::Failed);
            }
            if (last_pixel_submission_.value == std::numeric_limits<std::uint64_t>::max()) {
                return CompleteSubmit(SubmitResult::Failed);
            }
            const PixelSubmissionId submission{last_pixel_submission_.value + 1};
            frame_callback_ = wl_surface_frame(surface_);
            if (!frame_callback_) {
                return CompleteSubmit(SubmitResult::Failed);
            }
            frame_callback_submission_ = submission;
            static const wl_callback_listener listener{.done = FrameDone};
            wl_callback_add_listener(frame_callback_, &listener, this);
            if (!TrackPresentation(submission) && presentation_) {
                return CompleteSubmit(SubmitResult::Failed);
            }
            if (!commit_pixels_()) {
                return CompleteSubmit(SubmitResult::Failed);
            }
            ConfirmPixelSubmission(submission);
            committed_logical_size_ = metrics_.logical_size;
            mapped_ = true;
            force_pixels_ = state_pending_ = update_requested_ = false;
            return CompleteSubmit(SubmitResult::Pixels);
        }
        if (result == SubmitResult::State || state_pending_) {
            // State commits can pass an outstanding pixel frame callback. No
            // new frame callback or presentation feedback is requested here.
            wl_surface_commit(surface_);
            state_pending_ = false;
            if (allow_pixels) {
                update_requested_ = false;
            }
            return CompleteSubmit(SubmitResult::State);
        }
        if (allow_pixels) {
            update_requested_ = false;
        }
        return CompleteSubmit(SubmitResult::None);
    }
    if (state_pending_ && (!force_pixels_ || !allow_pixels)) {
        wl_surface_commit(surface_);
        state_pending_ = false;
        if (!force_pixels_) {
            update_requested_ = false;
        }
        return CompleteSubmit(SubmitResult::State);
    }
    if (!force_pixels_ || !allow_pixels || !paint_handler_) {
        return SubmitResult::None;
    }
    ReapBuffers();
    ShmBuffer *buffer = AcquireBuffer();
    if (!buffer) {
        return SubmitResult::None;
    }
    if (last_pixel_submission_.value == std::numeric_limits<std::uint64_t>::max()) {
        return CompleteSubmit(SubmitResult::Failed);
    }
    const PixelSubmissionId submission{last_pixel_submission_.value + 1};
    paint_handler_(buffer->pixels, buffer->width, buffer->height, buffer->width * 4);
    buffer->busy = true;
    wl_surface_attach(surface_, buffer->handle, 0, 0);
    wl_surface_damage_buffer(surface_, 0, 0, buffer->width, buffer->height);
    frame_callback_ = wl_surface_frame(surface_);
    if (!frame_callback_) {
        return CompleteSubmit(SubmitResult::Failed);
    }
    frame_callback_submission_ = submission;
    static const wl_callback_listener listener{.done = FrameDone};
    wl_callback_add_listener(frame_callback_, &listener, this);
    if (!TrackPresentation(submission) && presentation_) {
        return CompleteSubmit(SubmitResult::Failed);
    }
    wl_surface_commit(surface_);
    ConfirmPixelSubmission(submission);
    committed_logical_size_ = metrics_.logical_size;
    mapped_ = true;
    force_pixels_ = state_pending_ = update_requested_ = false;
    return CompleteSubmit(SubmitResult::Pixels);
}

void WaylandWindow::RequestRedraw(bool deferred)
{
    force_pixels_ = true;
    RequestUpdate(deferred);
}

void WaylandWindow::RequestUpdate(bool deferred)
{
    if (failed_) {
        return;
    }
    update_requested_ = true;
    if (!deferred) {
        TrySubmit();
    }
}

int WaylandWindow::DispatchPending()
{
    dispatching_ = true;
    const int result = wl_display_dispatch_pending(display_);
    FlushKeyboardFocus();
    dispatching_ = false;
    if (result < 0) {
        failed_ = true;
    }
    // A callback may close its owner or reject a submission. Disconnect after libwayland
    // finishes dispatching its queue, never from inside one of its listeners.
    if (failed_ || deferred_close_) {
        Close();
        return -1;
    }
    return result;
}

bool WaylandWindow::FailConnection()
{
    failed_ = true;
    Close();
    return false;
}

bool WaylandWindow::Pump(int timeout_ms, std::span<pollfd> wake_fds)
{
    for (auto &fd : wake_fds) {
        fd.revents = 0;
    }
    if (!display_ || failed_ || dispatching_ || closing_ || deferred_close_) {
        return false;
    }
    submission_deferred_ = false;
    if (TrySubmit() == SubmitResult::Failed || !display_ || close_requested_) {
        return false;
    }
    // Allocate before acquiring the read intention so exceptions cannot leave
    // an unmatched prepare_read behind.

    std::vector<pollfd> sources;
    sources.reserve(1 + wake_fds.size());
    sources.push_back({wl_display_get_fd(display_), POLLIN, 0});
    sources.insert(sources.end(), wake_fds.begin(), wake_fds.end());
    while (wl_display_prepare_read(display_) != 0) {
        const int dispatched = DispatchPending();
        if (dispatched < 0) {
            return false;
        }
        if (dispatched > 0) {
            // Actions may create a host descriptor or an earlier timer. Yield
            // before blocking with the caller's now outdated wait sources.
            if (TrySubmit() == SubmitResult::Failed || !display_ || close_requested_) {
                return false;
            }
            if (!submission_deferred_) {
                const int flushed = wl_display_flush(display_);
                if (flushed < 0 && errno != EAGAIN) {
                    return FailConnection();
                }
                return !close_requested_;
            }
            // A deferred preparation must also expose ready caller sources.
            // Acquire/cancel the read intention and poll them without waiting.
        }
    }
    const int flushed = wl_display_flush(display_);
    if (flushed < 0 && errno != EAGAIN) {
        wl_display_cancel_read(display_);
        return FailConnection();
    }
    sources.front().events = static_cast<short>(POLLIN | (flushed < 0 ? POLLOUT : 0));

    const auto wait_started = std::chrono::steady_clock::now();
    const int result = poll(sources.data(), sources.size(), submission_deferred_ ? 0 : timeout_ms);
    const int poll_error = errno;
    wait_duration_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - wait_started)
                             .count();
    errno = poll_error;
    const short revents = sources.front().revents;
    for (std::size_t i = 0; i < wake_fds.size(); ++i) {
        wake_fds[i].revents = sources[i + 1].revents;
    }
    if (result > 0 && (revents & (POLLERR | POLLHUP | POLLNVAL))) {
        wl_display_cancel_read(display_);
        return FailConnection();
    }
    if (result > 0 && (revents & POLLOUT) && wl_display_flush(display_) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(display_);
        return FailConnection();
    }
    if (result > 0 && (revents & POLLIN)) {
        if (wl_display_read_events(display_) < 0) {
            return FailConnection();
        }
    } else {
        wl_display_cancel_read(display_);
        if (result < 0 && errno != EINTR) {
            return FailConnection();
        }
    }
    if (DispatchPending() < 0) {
        return false;
    }
    ReapBuffers();
    if (TrySubmit() == SubmitResult::Failed) {
        return false;
    }
    return !close_requested_;
}

void WaylandWindow::RequestMaximize()
{
    if (toplevel_) {
        xdg_toplevel_set_maximized(toplevel_);
    }
}

WaylandSurfaceEffectCapabilities WaylandWindow::SurfaceEffectCapabilities() const noexcept
{
    if (!effect_manager_ || !surface_ || closing_ || deferred_close_ || failed_) {
        return {};
    }
    const auto version = prism_surface_effect_manager_v1_get_version(effect_manager_);
    return {backdrop_supported_, contour_supported_ && version >= 2,
            popup_backdrop_supported_ && version >= 3};
}

void WaylandWindow::SetSurfaceEffects(std::span<const contracts::SurfaceEffectRegion> regions)
{
    if (regions.size() > 8) {
        throw std::invalid_argument("Excessive surface effect regions");
    }

    std::vector<contracts::SurfaceEffectRegion> next;
    std::vector<std::vector<std::uint8_t>> payloads;
    next.reserve(regions.size());
    payloads.reserve(regions.size());
    const bool contour_supported =
        contour_supported_ && effect_manager_ &&
        prism_surface_effect_manager_v1_get_version(effect_manager_) >= 2;
    for (const auto &region : regions) {
        contracts::ValidateSurfaceEffectRegion(region);
        auto payload = region.contour ? contracts::EncodeContour(*region.contour)
                                      : std::vector<std::uint8_t>{};
        auto transmitted = region;
        transmitted.blur_radius = wl_fixed_to_double(wl_fixed_from_double(region.blur_radius));
        if (!region.contour) {
            transmitted.bounds = {wl_fixed_to_double(wl_fixed_from_double(region.bounds.x)),
                                  wl_fixed_to_double(wl_fixed_from_double(region.bounds.y)),
                                  wl_fixed_to_double(wl_fixed_from_double(region.bounds.width)),
                                  wl_fixed_to_double(wl_fixed_from_double(region.bounds.height))};
            transmitted.corner_radius =
                wl_fixed_to_double(wl_fixed_from_double(region.corner_radius));
        }
        contracts::ValidateSurfaceEffectRegion(transmitted);
        if (region.contour && !contour_supported) {
            continue;
        }
        next.push_back(std::move(transmitted));
        payloads.push_back(std::move(payload));
    }

    if (!surface_ || !effect_manager_ || !backdrop_supported_) {
        return;
    }
    if (next == sent_effects_) {
        return;
    }
    if (!surface_effect_) {
        surface_effect_ =
            prism_surface_effect_manager_v1_get_surface_effect(effect_manager_, surface_);
        if (!surface_effect_) {
            throw std::runtime_error("Cannot create surface effect object");
        }
    }

    prism_surface_effect_v1_clear(surface_effect_);
    for (std::size_t index = 0; index < next.size(); ++index) {
        const auto &region = next[index];
        if (region.contour) {
            auto &payload = payloads[index];
            wl_array array{payload.size(), 0, payload.data()};
            prism_surface_effect_v1_add_contour(surface_effect_,
                                                wl_fixed_from_double(region.blur_radius), &array);
            continue;
        }
        prism_surface_effect_v1_add_region(
            surface_effect_, wl_fixed_from_double(region.bounds.x),
            wl_fixed_from_double(region.bounds.y), wl_fixed_from_double(region.bounds.width),
            wl_fixed_from_double(region.bounds.height), wl_fixed_from_double(region.corner_radius),
            wl_fixed_from_double(region.blur_radius));
    }

    sent_effects_ = std::move(next);
    state_pending_ = update_requested_ = true;
}

void WaylandWindow::SetInputRegions(std::span<const contracts::SurfaceInputRegion> regions)
{
    if (!surface_ || !compositor_) {
        return;
    }
    std::vector<contracts::SurfaceInputRegion> next(regions.begin(), regions.end());
    if (input_sent_ && next == sent_input_) {
        return;
    }
    std::vector<contracts::LogicalRect> rectangles;
    const contracts::SurfaceInputRegion viewport{
        {0, 0, metrics_.logical_size.width, metrics_.logical_size.height}, 0};
    for (const auto &shape : next) {
        const contracts::SurfaceInputRegion intersection[]{shape, viewport};
        auto spans = contracts::RasterizeRoundedIntersection(intersection);
        rectangles.insert(rectangles.end(), spans.begin(), spans.end());
    }
    auto *input = wl_compositor_create_region(compositor_);
    for (const auto &rect : rectangles) {
        wl_region_add(input, static_cast<int>(rect.x), static_cast<int>(rect.y),
                      static_cast<int>(rect.width), static_cast<int>(rect.height));
    }
    wl_surface_set_input_region(surface_, input);
    wl_region_destroy(input);
    sent_input_ = std::move(next);
    input_sent_ = true;
    state_pending_ = update_requested_ = true;
}

void WaylandWindow::RunBeforeClose() noexcept
{
    auto handler = std::move(before_close_handler_);
    before_close_handler_ = {};
    if (handler) {
        handler();
    }
}

void WaylandWindow::Close()
{
    if (closing_) {
        return;
    }
    if (dispatching_) {
        if (!deferred_close_) {
            deferred_close_ = true;
            close_requested_ = true;
            closing_ = true;
            RunBeforeClose();
            ClosePopups(!destructing_);
            closing_ = false;
        }
        return;
    }
    closing_ = true;
    deferred_close_ = false;
    RunBeforeClose();
    ClosePopups(!destructing_);

    ReleasePointer();
    ReleaseKeyboard();
    ReleaseTouch();
    pending_keyboard_focus_.clear();

    for (auto &pending : feedbacks_) {
        if (pending.handle) {
            wp_presentation_feedback_destroy(pending.handle);
        }
        pending = {};
    }
    frame_callback_submission_ = {};
    if (presentation_) {
        wp_presentation_destroy(presentation_);
    }
    presentation_ = nullptr;
    if (frame_callback_) {
        wl_callback_destroy(frame_callback_);
    }
    frame_callback_ = nullptr;
    buffers_.clear();
    if (surface_effect_) {
        prism_surface_effect_v1_destroy(surface_effect_);
    }
    if (effect_manager_) {
        prism_surface_effect_manager_v1_destroy(effect_manager_);
    }
    surface_effect_ = nullptr;
    effect_manager_ = nullptr;
    backdrop_supported_ = false;
    contour_supported_ = false;
    popup_backdrop_supported_ = false;
    sent_effects_.clear();
    sent_input_.clear();
    input_sent_ = false;
    if (toplevel_) {
        xdg_toplevel_destroy(toplevel_);
    }
    if (xdg_surface_) {
        xdg_surface_destroy(xdg_surface_);
    }
    if (surface_) {
        wl_surface_destroy(surface_);
    }
    if (seat_) {
        wl_seat_release(seat_);
    }
    if (shell_) {
        xdg_wm_base_destroy(shell_);
    }
    if (shm_) {
        wl_shm_destroy(shm_);
    }
    if (compositor_) {
        wl_compositor_destroy(compositor_);
    }
    if (registry_) {
        wl_registry_destroy(registry_);
    }
    if (display_) {
        wl_display_disconnect(display_);
    }
    display_ = nullptr;
    registry_ = nullptr;
    compositor_ = nullptr;
    shm_ = nullptr;
    seat_ = nullptr;
    seat_global_name_ = 0;
    pointer_ = nullptr;
    keyboard_ = nullptr;
    shell_ = nullptr;
    surface_ = nullptr;
    xdg_surface_ = nullptr;
    toplevel_ = nullptr;
    configured_ = false;
    mapped_ = false;
    committed_logical_size_ = {};
    update_requested_ = force_pixels_ = state_pending_ = false;
    submission_deferred_ = false;
    closing_ = false;
}

} // namespace prism::platform
