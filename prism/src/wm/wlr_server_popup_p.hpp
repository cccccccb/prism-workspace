#pragma once

#include <wayland-server-core.h>

struct wlr_xdg_popup;
struct wlr_scene_tree;

namespace prism::wm {
class WlrServer;
struct WlrXdgView;

// Native protocol/scene ownership only. Popup content stays with its client.
struct WlrXdgPopup {
    WlrServer *server{};
    wlr_xdg_popup *native{};
    WlrXdgView *owner{};
    wlr_scene_tree *tree{};
    wl_event_source *dismiss_idle{};
    wl_listener commit{}, map{}, unmap{}, destroy{}, parent_destroy{}, reposition{}, tree_destroy{};

    WlrXdgPopup();
    ~WlrXdgPopup();
    static void Commit(wl_listener *, void *);
    static void Map(wl_listener *, void *);
    static void Unmap(wl_listener *, void *);
    static void Destroy(wl_listener *, void *);
    static void ParentGone(wl_listener *, void *);
    static void Reposition(wl_listener *, void *);
    static void TreeGone(wl_listener *, void *);
    static void Dismiss(void *);
};

void handle_server_new_xdg_popup(wl_listener *, void *);
} // namespace prism::wm
