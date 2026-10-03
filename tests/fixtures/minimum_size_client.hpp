#pragma once
#include "native_layout_fixture.hpp"
#include "xdg-shell-client-protocol.h"
#include <cstring>
#include <sys/mman.h>

namespace {
// A real external XDG client: constraints are committed over the protocol, not
// written into the compositor's Window by the test.
class MinimumSizeClient {
public:
    explicit MinimumSizeClient(int channel) : channel_(channel)
    {
    }

    int Run(const char *socket)
    {
        char start{};
        assert(recv(channel_, &start, 1, 0) == 1 && start == 'S');
        display_ = wl_display_connect(socket);
        assert(display_);
        auto *registry = wl_display_get_registry(display_);
        static const wl_registry_listener registry_listener{Global, GlobalRemove};
        wl_registry_add_listener(registry, &registry_listener, this);
        assert(wl_display_roundtrip(display_) >= 0 && compositor_ && shm_ && wm_);
        static const xdg_wm_base_listener wm_listener{Ping};
        xdg_wm_base_add_listener(wm_, &wm_listener, this);
        surface_ = wl_compositor_create_surface(compositor_);
        xdg_ = xdg_wm_base_get_xdg_surface(wm_, surface_);
        static const xdg_surface_listener surface_listener{Configure};
        xdg_surface_add_listener(xdg_, &surface_listener, this);
        top_ = xdg_surface_get_toplevel(xdg_);
        static const xdg_toplevel_listener top_listener{Size, Close, Bounds, Capabilities};
        xdg_toplevel_add_listener(top_, &top_listener, this);
        xdg_toplevel_set_app_id(top_, "prism.minimum-fixture");
        xdg_toplevel_set_min_size(top_, 240, 120);
        wl_surface_commit(surface_);
        assert(wl_display_roundtrip(display_) >= 0);
        assert(wl_display_roundtrip(display_) >= 0);
        Send({PacketKind::Ready, {}});

        for (;;) {
            assert(wl_display_dispatch_pending(display_) >= 0);
            assert(wl_display_flush(display_) >= 0 || errno == EAGAIN);
            pollfd fds[]{{channel_, POLLIN, 0}, {wl_display_get_fd(display_), POLLIN, 0}};
            assert(poll(fds, 2, -1) >= 0);
            if (fds[0].revents & POLLIN) {
                char command{};
                assert(recv(channel_, &command, 1, 0) == 1);
                if (command == 'Q') {
                    break;
                }
                if (command == 'M') {
                    xdg_toplevel_set_min_size(top_, 2000, 120);
                    wl_surface_commit(surface_);
                } else {
                    assert(command == 'B');
                }
                assert(wl_display_roundtrip(display_) >= 0);
                Send({PacketKind::Barrier, {}, 0, 0, configures_});
                continue;
            } else if (fds[0].revents & (POLLHUP | POLLERR)) {
                break;
            }
            if (fds[1].revents & POLLIN) {
                assert(wl_display_dispatch(display_) >= 0);
            }
        }
        wl_registry_destroy(registry);
        wl_display_disconnect(display_);
        close(channel_);
        return 0;
    }

private:
    void Send(Packet packet)
    {
        assert(send(channel_, &packet, sizeof(packet), MSG_NOSIGNAL) == sizeof(packet));
    }

    static void Global(void *data, wl_registry *registry, uint32_t name, const char *interface,
                       uint32_t)
    {
        auto &self = *static_cast<MinimumSizeClient *>(data);
        if (!std::strcmp(interface, "wl_compositor")) {
            self.compositor_ = static_cast<wl_compositor *>(
                wl_registry_bind(registry, name, &wl_compositor_interface, 4));
        } else if (!std::strcmp(interface, "wl_shm")) {
            self.shm_ =
                static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
        } else if (!std::strcmp(interface, "xdg_wm_base")) {
            self.wm_ = static_cast<xdg_wm_base *>(
                wl_registry_bind(registry, name, &xdg_wm_base_interface, 3));
        }
    }

    static void GlobalRemove(void *, wl_registry *, uint32_t)
    {
    }

    static void Ping(void *, xdg_wm_base *wm, uint32_t serial)
    {
        xdg_wm_base_pong(wm, serial);
    }

    static void Size(void *data, xdg_toplevel *, int32_t width, int32_t height, wl_array *)
    {
        auto &self = *static_cast<MinimumSizeClient *>(data);
        self.width_ = std::max(1, width);
        self.height_ = std::max(1, height);
    }

    static void Close(void *, xdg_toplevel *)
    {
    }

    static void Bounds(void *, xdg_toplevel *, int32_t, int32_t)
    {
    }

    static void Capabilities(void *, xdg_toplevel *, wl_array *)
    {
    }

    static void Release(void *, wl_buffer *buffer)
    {
        wl_buffer_destroy(buffer);
    }

    static void Configure(void *data, xdg_surface *xdg, uint32_t serial)
    {
        auto &self = *static_cast<MinimumSizeClient *>(data);
        ++self.configures_;
        xdg_surface_ack_configure(xdg, serial);
        const int bytes = self.width_ * self.height_ * 4;
        const int fd = memfd_create("prism-minimum-test", MFD_CLOEXEC);
        assert(fd >= 0 && ftruncate(fd, bytes) == 0);
        auto *pool = wl_shm_create_pool(self.shm_, fd, bytes);
        auto *buffer = wl_shm_pool_create_buffer(pool, 0, self.width_, self.height_,
                                                 self.width_ * 4, WL_SHM_FORMAT_ARGB8888);
        static const wl_buffer_listener listener{Release};
        wl_buffer_add_listener(buffer, &listener, nullptr);
        wl_shm_pool_destroy(pool);
        close(fd);
        wl_surface_attach(self.surface_, buffer, 0, 0);
        wl_surface_damage_buffer(self.surface_, 0, 0, self.width_, self.height_);
        wl_surface_commit(self.surface_);
    }

    int channel_;
    wl_display *display_{};
    wl_compositor *compositor_{};
    wl_shm *shm_{};
    xdg_wm_base *wm_{};
    wl_surface *surface_{};
    xdg_surface *xdg_{};
    xdg_toplevel *top_{};
    int width_{480}, height_{300};
    std::uint32_t configures_{};
};
} // namespace
