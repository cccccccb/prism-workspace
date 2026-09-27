#pragma once
#include "prism/core/logging.hpp"
#include "prism/decoration/tiling_decoration_spec.hpp"
#include "prism/decoration/tiling_drag_manager.hpp"
#include "prism/decoration/tiling_window_decorator.hpp"
#include "prism/ipc/channel.hpp"
#include "prism/ipc/ipc_server.hpp"
#include "prism/layout/fluid_split_strategy.hpp"
#include "prism/layout/mission_control_strategy.hpp"
#include "prism/wm/surface_effects.hpp"
#include "prism/wm/wlr_server.hpp"
#include "prism/wm/xdg_view.hpp"

extern "C" {
#include <drm_fourcc.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/wayland.h>
#include <wlr/backend/x11.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <ctime>
#include <optional>
#include <poll.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <functional>

namespace prism::wm {
// Type-safe container_of for C++
template <typename Parent, typename Member> inline Parent *WlContainerOf(Member *ptr, size_t offset)
{
    return reinterpret_cast<Parent *>(reinterpret_cast<char *>(ptr) - offset);
}

// Native keyboard listener ownership.
struct WlrKeyboardBinding {
    WlrServer *server{nullptr};
    struct wlr_keyboard *keyboard{nullptr};
    struct wl_listener key{};
    struct wl_listener modifiers{};
    struct wl_listener destroy{};
    std::array<bool, 768> consumed_keys{};

    ~WlrKeyboardBinding()
    {
        wl_list_remove(&key.link);
        wl_list_remove(&modifiers.link);
        wl_list_remove(&destroy.link);
    }
};

struct WlrSurfaceWatch {
    struct ChildState {
        wlr_subsurface *child{};
        std::int32_t x{}, y{};
        bool above{};
        bool operator==(const ChildState &) const = default;
    };

    WlrServer *server{};
    wlr_surface *surface{};
    wl_listener commit{}, map{}, unmap{}, destroy{};
    std::vector<ChildState> children;
    wlr_box local_geometry{};
    bool has_local_geometry{};

    bool CompareChildren(wl_list *list, bool above, std::size_t &index) const
    {
        bool changed = false;
        wlr_subsurface *child;
        wl_list_for_each(child, list, current.link)
        {
            const ChildState value{child, child->current.x, child->current.y, above};
            if (index >= children.size() || children[index] != value) {
                changed = true;
            }
            ++index;
        }
        return changed;
    }

    void RecordChildren(wl_list *list, bool above)
    {
        wlr_subsurface *child;
        wl_list_for_each(child, list, current.link)
        {
            children.push_back({child, child->current.x, child->current.y, above});
        }
    }

    bool RefreshChildren()
    {
        std::size_t index = 0;
        bool changed = CompareChildren(&surface->current.subsurfaces_below, false, index);
        changed |= CompareChildren(&surface->current.subsurfaces_above, true, index);
        if (!changed && index == children.size()) {
            return false;
        }

        children.clear();
        RecordChildren(&surface->current.subsurfaces_below, false);
        RecordChildren(&surface->current.subsurfaces_above, true);
        return true;
    }

    bool RefreshLocalGeometry()
    {
        auto *xdg = wlr_xdg_surface_try_from_wlr_surface(surface);
        if (!xdg) {
            const bool changed = has_local_geometry;
            has_local_geometry = false;
            return changed;
        }
        wlr_box next{};
        wlr_xdg_surface_get_geometry(xdg, &next);
        const bool changed = !has_local_geometry || local_geometry.x != next.x ||
                             local_geometry.y != next.y || local_geometry.width != next.width ||
                             local_geometry.height != next.height;
        local_geometry = next;
        has_local_geometry = true;
        return changed;
    }

    WlrSurfaceWatch(WlrServer *owner, wlr_surface *value) : server(owner), surface(value)
    {
        commit.notify = Commit;
        destroy.notify = Destroy;
        map.notify = Map;
        unmap.notify = Unmap;
        wl_signal_add(&surface->events.commit, &commit);
        wl_signal_add(&surface->events.map, &map);
        wl_signal_add(&surface->events.unmap, &unmap);
        wl_signal_add(&surface->events.destroy, &destroy);
    }

    static void Commit(wl_listener *listener, void *)
    {
        auto *watch = WlContainerOf<WlrSurfaceWatch>(listener, offsetof(WlrSurfaceWatch, commit));
        watch->server->HandleSurfaceCommit(watch->surface);
    }

    static void Destroy(wl_listener *listener, void *)
    {
        auto *watch = WlContainerOf<WlrSurfaceWatch>(listener, offsetof(WlrSurfaceWatch, destroy));
        watch->server->HandleSurfaceDestroy(watch->surface);
    }

    static void Map(wl_listener *listener, void *)
    {
        auto *watch = WlContainerOf<WlrSurfaceWatch>(listener, offsetof(WlrSurfaceWatch, map));
        watch->server->HandleSurfaceMapState(watch->surface);
    }

    static void Unmap(wl_listener *listener, void *)
    {
        auto *watch = WlContainerOf<WlrSurfaceWatch>(listener, offsetof(WlrSurfaceWatch, unmap));
        watch->server->HandleSurfaceMapState(watch->surface);
    }

    ~WlrSurfaceWatch()
    {
        wl_list_remove(&commit.link);
        wl_list_remove(&map.link);
        wl_list_remove(&unmap.link);
        wl_list_remove(&destroy.link);
    }
};

inline void UpdateCommittedGeometry(WlrXdgView *view)
{
    if (!view->managed || !view->mapped) {
        return;
    }
    // xdg_surface.current.geometry only contains an explicitly supplied client
    // rectangle. The effective geometry also supports clients that omit it.
    wlr_box geometry{};
    wlr_xdg_surface_get_geometry(view->toplevel->base, &geometry);
    // wlr_scene_xdg_surface already compensates for the local geometry origin,
    // so the view position is the global origin of this effective rectangle.
    view->managed->SetCommittedBounds({static_cast<float>(view->x), static_cast<float>(view->y),
                                       static_cast<float>(geometry.width),
                                       static_cast<float>(geometry.height)});
}

void handle_output_frame(struct wl_listener *listener, void *data);
void handle_output_present(struct wl_listener *listener, void *data);
void handle_output_commit(wl_listener *listener, void *data);
void handle_output_needs_frame(wl_listener *listener, void *);
void handle_output_damage(wl_listener *listener, void *);
void handle_server_new_surface(wl_listener *listener, void *data);
void handle_output_destroy(struct wl_listener *listener, void *data);
void handle_server_new_xdg_surface(struct wl_listener *listener, void *data);
void handle_xdg_map(struct wl_listener *listener, void *);
void handle_xdg_unmap(struct wl_listener *listener, void *);
void handle_xdg_destroy(struct wl_listener *listener, void *);
void handle_xdg_maximize(struct wl_listener *listener, void *);
void handle_xdg_fullscreen(struct wl_listener *listener, void *);
void handle_keyboard_key(struct wl_listener *listener, void *data);
void handle_keyboard_modifiers(struct wl_listener *listener, void *);
void handle_keyboard_destroy(struct wl_listener *listener, void *);
void handle_server_new_output(struct wl_listener *listener, void *data);
void handle_server_new_input(struct wl_listener *listener, void *data);
void handle_cursor_motion(struct wl_listener *listener, void *data);
void handle_cursor_motion_absolute(struct wl_listener *listener, void *data);
void handle_cursor_button(struct wl_listener *listener, void *data);
void handle_cursor_axis(struct wl_listener *listener, void *data);
} // namespace prism::wm
