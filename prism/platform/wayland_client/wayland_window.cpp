#include "presentation-time-client-protocol.h"
#include "prism-surface-effects-client.h"
#include "wayland_window_p.hpp"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

namespace prism::platform {

WaylandWindow::WaylandWindow() = default;

WaylandWindow::~WaylandWindow()
{
    Close();
}

void WaylandWindow::Emit(contracts::WindowEvent event)
{
    if (event_handler_) {
        event_handler_(event);
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

struct wp_presentation_feedback *WaylandWindow::TrackPresentation()
{
    if (!presentation_ || feedbacks_.size() >= 8) {
        return nullptr;
    }
    auto *feedback = wp_presentation_feedback(presentation_, surface_);
    static const wp_presentation_feedback_listener listener{.sync_output = PresentationOutput,
                                                            .presented = PresentationDone,
                                                            .discarded = PresentationDiscarded};
    wp_presentation_feedback_add_listener(feedback, &listener, this);
    feedbacks_.push_back(feedback);
    return feedback;
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
    }
    if (on_submitted_) {
        on_submitted_(result);
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
    if (!configured_ || !update_requested_ || !surface_) {
        return SubmitResult::None;
    }
    const bool allow_pixels = frame_callback_ == nullptr;
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
        if (force_pixels_ && allow_pixels && result != SubmitResult::Pixels) {
            return CompleteSubmit(SubmitResult::Failed);
        }
        if (result == SubmitResult::Pixels) {
            if (!allow_pixels || !commit_pixels_) {
                return CompleteSubmit(SubmitResult::Failed);
            }
            frame_callback_ = wl_surface_frame(surface_);
            static const wl_callback_listener listener{.done = FrameDone};
            wl_callback_add_listener(frame_callback_, &listener, this);
            TrackPresentation();
            if (!commit_pixels_()) {
                return CompleteSubmit(SubmitResult::Failed);
            }
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
    paint_handler_(buffer->pixels, buffer->width, buffer->height, buffer->width * 4);
    buffer->busy = true;
    wl_surface_attach(surface_, buffer->handle, 0, 0);
    wl_surface_damage_buffer(surface_, 0, 0, buffer->width, buffer->height);
    frame_callback_ = wl_surface_frame(surface_);
    static const wl_callback_listener listener{.done = FrameDone};
    wl_callback_add_listener(frame_callback_, &listener, this);
    TrackPresentation();
    wl_surface_commit(surface_);
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

bool WaylandWindow::Open(const std::string &socket_name, const std::string &app_id,
                         const std::string &title, int preferred_width, int preferred_height)
{
    if (display_) {
        return false;
    }
    failed_ = false;
    preferred_width_ = std::clamp(preferred_width, 1, 4096);
    preferred_height_ = std::clamp(preferred_height, 1, 4096);
    display_ = wl_display_connect(socket_name.empty() ? nullptr : socket_name.c_str());

    if (!display_) {
        return false;
    }
    registry_ = wl_display_get_registry(display_);
    static const wl_registry_listener registry_listener{.global = RegistryGlobal,
                                                        .global_remove = RegistryGlobalRemove};
    wl_registry_add_listener(registry_, &registry_listener, this);
    if (wl_display_roundtrip(display_) < 0 || wl_display_roundtrip(display_) < 0 || !compositor_ ||
        !shm_ || !shell_) {
        Close();
        return false;
    }
    surface_ = wl_compositor_create_surface(compositor_);
    if (surface_) {
        xdg_surface_ = xdg_wm_base_get_xdg_surface(shell_, surface_);
    }
    if (xdg_surface_) {
        toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
    }
    if (!surface_ || !xdg_surface_ || !toplevel_) {
        Close();
        return false;
    }
    static const xdg_surface_listener surface_listener{.configure = SurfaceConfigure};
    static const xdg_toplevel_listener toplevel_listener{.configure = ToplevelConfigure,
                                                         .close = ToplevelClose,
                                                         .configure_bounds =
                                                             ToplevelConfigureBounds,
                                                         .wm_capabilities = ToplevelCapabilities};
    xdg_surface_add_listener(xdg_surface_, &surface_listener, this);
    xdg_toplevel_add_listener(toplevel_, &toplevel_listener, this);
    xdg_toplevel_set_app_id(toplevel_, app_id.c_str());
    xdg_toplevel_set_title(toplevel_, title.c_str());
    wl_surface_commit(surface_); // Required empty initial commit before any buffer.
    return wl_display_flush(display_) >= 0 || errno == EAGAIN;
}

int WaylandWindow::DispatchPending()
{
    dispatching_ = true;
    const int result = wl_display_dispatch_pending(display_);
    dispatching_ = false;
    // A callback may reject a submission. Disconnect only after libwayland
    // finishes dispatching its queue, never from inside one of its listeners.
    if (failed_) {
        Close();
        return -1;
    }
    return result;
}

bool WaylandWindow::Pump(int timeout_ms, std::span<pollfd> wake_fds)
{
    for (auto &fd : wake_fds) {
        fd.revents = 0;
    }
    if (!display_ || failed_) {
        return false;
    }
    if (TrySubmit() == SubmitResult::Failed) {
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
            if (TrySubmit() == SubmitResult::Failed) {
                return false;
            }
            const int flushed = wl_display_flush(display_);
            return (flushed >= 0 || errno == EAGAIN) && !close_requested_;
        }
    }
    const int flushed = wl_display_flush(display_);
    if (flushed < 0 && errno != EAGAIN) {
        wl_display_cancel_read(display_);
        return false;
    }
    sources.front().events = static_cast<short>(POLLIN | (flushed < 0 ? POLLOUT : 0));

    const int result = poll(sources.data(), sources.size(), timeout_ms);
    const short revents = sources.front().revents;
    for (std::size_t i = 0; i < wake_fds.size(); ++i) {
        wake_fds[i].revents = sources[i + 1].revents;
    }
    if (result > 0 && (revents & (POLLERR | POLLHUP | POLLNVAL))) {
        wl_display_cancel_read(display_);
        return false;
    }
    if (result > 0 && (revents & POLLOUT) && wl_display_flush(display_) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(display_);
        return false;
    }
    if (result > 0 && (revents & POLLIN)) {
        if (wl_display_read_events(display_) < 0) {
            return false;
        }
    } else {
        wl_display_cancel_read(display_);
        if (result < 0 && errno != EINTR) {
            return false;
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

void WaylandWindow::SetSurfaceEffects(std::span<const contracts::SurfaceEffectRegion> regions)
{
    if (!surface_ || !effect_manager_ || !backdrop_supported_) {
        return;
    }
    std::vector<contracts::SurfaceEffectRegion> next(regions.begin(), regions.end());
    if (next == sent_effects_) {
        return;
    }
    if (!surface_effect_) {
        surface_effect_ =
            prism_surface_effect_manager_v1_get_surface_effect(effect_manager_, surface_);
    }
    prism_surface_effect_v1_clear(surface_effect_);
    for (const auto &region : next) {
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
    auto *input = wl_compositor_create_region(compositor_);
    for (const auto &shape : next) {
        const auto &b = shape.bounds;
        const double radius = std::clamp(shape.corner_radius, 0.0, std::min(b.width, b.height) / 2);
        const int first = std::max(0, int(std::ceil(b.y))),
                  last =
                      std::min(int(metrics_.logical_size.height), int(std::floor(b.y + b.height)));
        for (int y = first; y < last; ++y) {
            double inset = 0;
            const double edge = std::min(y + .5 - b.y, b.y + b.height - y - .5);
            if (edge < radius) {
                inset =
                    radius -
                    std::sqrt(std::max(0.0, radius * radius - (radius - edge) * (radius - edge)));
            }
            const int left = std::max(0, int(std::ceil(b.x + inset))),
                      right = std::min(int(metrics_.logical_size.width),
                                       int(std::floor(b.x + b.width - inset)));
            if (right > left) {
                wl_region_add(input, left, y, right - left, 1);
            }
        }
    }
    wl_surface_set_input_region(surface_, input);
    wl_region_destroy(input);
    sent_input_ = std::move(next);
    input_sent_ = true;
    state_pending_ = update_requested_ = true;
}

void WaylandWindow::Close()
{
    for (auto *feedback : feedbacks_) {
        wp_presentation_feedback_destroy(feedback);
    }
    feedbacks_.clear();
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
    if (pointer_) {
        wl_pointer_release(pointer_);
    }
    if (keyboard_) {
        wl_keyboard_release(keyboard_);
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
    update_requested_ = force_pixels_ = state_pending_ = false;
}

} // namespace prism::platform
