#pragma once

#include "prism/wm/window.hpp"
#include <cstdint>
#include <memory>
#include <sys/types.h>
#include <wayland-server-core.h>

struct wlr_xdg_toplevel;
struct wlr_scene_tree;
namespace prism::wm {
class WlrServer;

// One lifecycle record for a real Wayland toplevel. Geometry is a configure
// target; the last committed size is tracked independently by the Window.
struct WlrXdgView {
    WlrServer* server{};
    wlr_xdg_toplevel* toplevel{};
    wlr_scene_tree* scene_tree{};
    wl_listener map{}, commit{}, unmap{}, destroy{}, request_maximize{}, request_fullscreen{};
    wl_listener set_title{}, set_app_id{};
    std::shared_ptr<Window> managed;
    int x{}, y{}, width{640}, height{400};
    bool mapped{}, visible{}, fullscreen{}, maximized{};
    std::uint32_t tiled_edges{~std::uint32_t{0}};
    std::uint64_t instance{};
    pid_t pid{};
    int shell_role{}; // 0 ordinary, 1 desktop, 2 topbar, 3 dock
    WlrXdgView();
    ~WlrXdgView();
};
}
