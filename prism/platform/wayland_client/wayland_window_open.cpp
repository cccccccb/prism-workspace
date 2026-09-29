#include "prism/platform/wayland_window.hpp"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <poll.h>

namespace prism::platform {

namespace {

bool OpenStopped(const WaylandOpenOptions &options)
{
    if (std::chrono::steady_clock::now() >= options.deadline) {
        return true;
    }
    if (options.cancel_fd < 0) {
        return false;
    }

    pollfd source{options.cancel_fd, POLLIN, 0};
    const int ready = poll(&source, 1, 0);
    if (ready < 0) {
        return errno != EINTR;
    }
    return ready > 0 && (source.revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL));
}

int OpenTimeoutMs(const WaylandOpenOptions &options)
{
    if (options.deadline == std::chrono::steady_clock::time_point::max()) {
        return -1;
    }

    const auto remaining = options.deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
        return 0;
    }

    const auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
    return static_cast<int>(std::min<std::int64_t>(milliseconds, INT_MAX));
}

} // namespace

bool WaylandWindow::Open(const std::string &socket_name, const std::string &app_id,
                         const std::string &title, int preferred_width, int preferred_height)
{
    const WaylandOpenOptions options{-1,
                                     std::chrono::steady_clock::now() + std::chrono::seconds(10)};
    return Open(socket_name, app_id, title, preferred_width, preferred_height, options);
}

void WaylandWindow::OpenSyncDone(void *data, wl_callback *callback, std::uint32_t)
{
    *static_cast<bool *>(data) = true;
    wl_callback_destroy(callback);
}

bool WaylandWindow::WaitForOpenSync(bool &done, const WaylandOpenOptions &options)
{
    std::array<pollfd, 2> sources{
        {{wl_display_get_fd(display_), POLLIN, 0}, {options.cancel_fd, POLLIN, 0}}};
    const nfds_t source_count = options.cancel_fd >= 0 ? 2 : 1;

    while (!done && !OpenStopped(options)) {
        while (wl_display_prepare_read(display_) != 0) {
            if (DispatchPending() < 0) {
                return false;
            }
            if (done || OpenStopped(options)) {
                return done && !OpenStopped(options);
            }
        }

        const int flushed = wl_display_flush(display_);
        if (flushed < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display_);
            return false;
        }
        sources[0].events = static_cast<short>(POLLIN | (flushed < 0 ? POLLOUT : 0));
        sources[0].revents = 0;
        sources[1].revents = 0;

        const int ready = poll(sources.data(), source_count, OpenTimeoutMs(options));
        const int poll_error = errno;
        if (ready < 0 ||
            (ready > 0 && source_count == 2 &&
             (sources[1].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) ||
            OpenStopped(options)) {
            wl_display_cancel_read(display_);
            if (ready < 0 && poll_error == EINTR && !OpenStopped(options)) {
                continue;
            }
            return false;
        }
        if (ready > 0 && (sources[0].revents & (POLLERR | POLLHUP | POLLNVAL))) {
            wl_display_cancel_read(display_);
            return false;
        }
        if (ready > 0 && (sources[0].revents & POLLOUT) && wl_display_flush(display_) < 0 &&
            errno != EAGAIN) {
            wl_display_cancel_read(display_);
            return false;
        }
        if (ready > 0 && (sources[0].revents & POLLIN)) {
            if (wl_display_read_events(display_) < 0) {
                return false;
            }
        } else {
            wl_display_cancel_read(display_);
            if (ready == 0) {
                return false;
            }
        }
        if (DispatchPending() < 0) {
            return false;
        }
    }

    return done && !OpenStopped(options);
}

bool WaylandWindow::OpenRoundtrip(const WaylandOpenOptions &options)
{
    auto *sync = wl_display_sync(display_);
    if (!sync) {
        return false;
    }
    bool done = false;
    static const wl_callback_listener listener{.done = OpenSyncDone};
    if (wl_callback_add_listener(sync, &listener, &done) < 0) {
        wl_callback_destroy(sync);
        return false;
    }

    const bool completed = WaitForOpenSync(done, options);
    if (!done && display_) {
        wl_callback_destroy(sync);
    }
    return completed;
}

bool WaylandWindow::Open(const std::string &socket_name, const std::string &app_id,
                         const std::string &title, int preferred_width, int preferred_height,
                         WaylandOpenOptions options)
{
    if (display_ || OpenStopped(options)) {
        return false;
    }
    failed_ = false;
    preferred_width_ = std::clamp(preferred_width, 1, 4096);
    preferred_height_ = std::clamp(preferred_height, 1, 4096);
    display_ = wl_display_connect(socket_name.empty() ? nullptr : socket_name.c_str());

    if (!display_ || OpenStopped(options)) {
        Close();
        return false;
    }
    registry_ = wl_display_get_registry(display_);
    static const wl_registry_listener registry_listener{.global = RegistryGlobal,
                                                        .global_remove = RegistryGlobalRemove};
    if (!registry_ || wl_registry_add_listener(registry_, &registry_listener, this) < 0 ||
        !OpenRoundtrip(options) || !OpenRoundtrip(options) || !compositor_ || !shm_ || !shell_ ||
        OpenStopped(options)) {
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
    const int flushed = wl_display_flush(display_);
    if ((flushed < 0 && errno != EAGAIN) || OpenStopped(options)) {
        Close();
        return false;
    }
    return true;
}

} // namespace prism::platform
