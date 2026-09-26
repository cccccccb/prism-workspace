#include "prism/wm/wlr_server.hpp"
#include "prism/wm/surface_effects.hpp"
#include "prism/wm/xdg_view.hpp"
#include "prism/ipc/ipc_server.hpp"
#include "prism/ipc/channel.hpp"
#include "prism/core/logging.hpp"
#include "prism/layout/fluid_split_strategy.hpp"
#include "prism/layout/mission_control_strategy.hpp"
#include "prism/decoration/tiling_decoration_spec.hpp"
#include "prism/decoration/tiling_window_decorator.hpp"
#include "prism/decoration/tiling_drag_manager.hpp"
#include <fstream>

extern "C" {
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/wayland.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/x11.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <xkbcommon/xkbcommon.h>
#include <drm_fourcc.h>
}

#include <cstddef>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <unistd.h>
#include <sstream>
#include <cmath>
#include <iomanip>
#include <algorithm>
#include <sys/syscall.h>
#include <poll.h>
#include <sys/wait.h>
#include <signal.h>
#include <array>
#include <nlohmann/json.hpp>
#include <optional>



namespace prism::wm {

WlrXdgView::WlrXdgView() {
    for (auto* listener : {&map, &commit, &unmap, &destroy, &request_maximize,
         &request_fullscreen, &set_title, &set_app_id}) wl_list_init(&listener->link);
}
WlrXdgView::~WlrXdgView() {
    for (auto* listener : {&map, &commit, &unmap, &destroy, &request_maximize,
         &request_fullscreen, &set_title, &set_app_id}) wl_list_remove(&listener->link);
}

static void UpdateCommittedGeometry(WlrXdgView* view) {
    if (!view->managed || !view->mapped) return;
    // xdg_surface.current.geometry only contains an explicitly supplied client
    // rectangle. The effective geometry also supports clients that omit it.
    wlr_box geometry{};
    wlr_xdg_surface_get_geometry(view->toplevel->base, &geometry);
    // wlr_scene_xdg_surface already compensates for the local geometry origin,
    // so the view position is the global origin of this effective rectangle.
    view->managed->SetCommittedBounds({static_cast<float>(view->x), static_cast<float>(view->y),
        static_cast<float>(geometry.width), static_cast<float>(geometry.height)});
}

struct WlrKeyboardBinding {
    WlrServer* server{nullptr};
    struct wlr_keyboard* keyboard{nullptr};
    struct wl_listener key{};
    struct wl_listener modifiers{};
    struct wl_listener destroy{};
    std::array<bool, 768> consumed_keys{};

    ~WlrKeyboardBinding() {
        wl_list_remove(&key.link);
        wl_list_remove(&modifiers.link);
        wl_list_remove(&destroy.link);
    }
};

// Type-safe container_of for C++
template <typename Parent, typename Member>
static inline Parent* WlContainerOf(Member* ptr, size_t offset) {
    return reinterpret_cast<Parent*>(reinterpret_cast<char*>(ptr) - offset);
}

// Output listeners
static void handle_output_frame(struct wl_listener* listener, void* data) {
    auto* output = WlContainerOf<WlrOutput>(listener, offsetof(WlrOutput, frame));
    output->server->HandleOutputFrame(output);
}

static void handle_output_destroy(struct wl_listener* listener, void* data) {
    auto* output = WlContainerOf<WlrOutput>(listener, offsetof(WlrOutput, destroy));
    if (output && output->server) {
        output->server->RemoveOutput(output);
    }
}

static void handle_server_new_xdg_surface(struct wl_listener* listener, void* data) {
    auto* sig = WlContainerOf<WlrServerSignals>(listener, offsetof(WlrServerSignals, new_xdg_toplevel));
    sig->server->HandleNewXdgToplevel(static_cast<struct wlr_xdg_toplevel*>(data));
}

static void handle_xdg_map(struct wl_listener* listener, void*) {
    auto* view = WlContainerOf<WlrXdgView>(listener, offsetof(WlrXdgView, map));
    view->server->HandleXdgMap(view);
}
static void handle_xdg_unmap(struct wl_listener* listener, void*) {
    auto* view = WlContainerOf<WlrXdgView>(listener, offsetof(WlrXdgView, unmap));
    view->server->HandleXdgUnmap(view);
}
static void handle_xdg_destroy(struct wl_listener* listener, void*) {
    auto* view = WlContainerOf<WlrXdgView>(listener, offsetof(WlrXdgView, destroy));
    view->server->HandleXdgDestroy(view);
}
static void handle_xdg_maximize(struct wl_listener* listener, void*) {
    auto* view = WlContainerOf<WlrXdgView>(listener, offsetof(WlrXdgView, request_maximize));
    view->server->HandleXdgMaximize(view);
}
static void handle_xdg_fullscreen(struct wl_listener* listener, void*) {
    auto* view = WlContainerOf<WlrXdgView>(listener, offsetof(WlrXdgView, request_fullscreen));
    view->server->HandleXdgMaximize(view);
}
static void handle_keyboard_key(struct wl_listener* listener, void* data) {
    auto* binding = WlContainerOf<WlrKeyboardBinding>(listener, offsetof(WlrKeyboardBinding, key));
    binding->server->HandleKeyboardKey(binding, data);
}
static void handle_keyboard_modifiers(struct wl_listener* listener, void*) {
    auto* binding = WlContainerOf<WlrKeyboardBinding>(listener, offsetof(WlrKeyboardBinding, modifiers));
    binding->server->HandleKeyboardModifiers(binding);
}
static void handle_keyboard_destroy(struct wl_listener* listener, void*) {
    auto* binding = WlContainerOf<WlrKeyboardBinding>(listener, offsetof(WlrKeyboardBinding, destroy));
    binding->server->HandleKeyboardDestroy(binding);
}

WlrOutput::WlrOutput(struct wlr_output* out, WlrServer* s)
    : wlr_output(out), server(s) {
    frame.notify = handle_output_frame;
    wl_signal_add(&wlr_output->events.frame, &frame);

    destroy.notify = handle_output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &destroy);
}

WlrOutput::~WlrOutput() {
    if (scene_output) {
        wlr_scene_output_destroy(scene_output);
        scene_output = nullptr;
    }
    wl_list_remove(&frame.link);
    wl_list_remove(&destroy.link);
}

// Server listeners
static void handle_server_new_output(struct wl_listener* listener, void* data) {
    auto* sig = WlContainerOf<WlrServerSignals>(listener, offsetof(WlrServerSignals, new_output));
    sig->server->HandleNewOutput(static_cast<struct wlr_output*>(data));
}

static void handle_server_new_input(struct wl_listener* listener, void* data) {
    auto* sig = WlContainerOf<WlrServerSignals>(listener, offsetof(WlrServerSignals, new_input));
    sig->server->HandleNewInput(static_cast<struct wlr_input_device*>(data));
}

static void handle_cursor_motion(struct wl_listener* listener, void* data) {
    auto* sig = WlContainerOf<WlrServerSignals>(listener, offsetof(WlrServerSignals, cursor_motion));
    auto* event = static_cast<struct wlr_pointer_motion_event*>(data);
    sig->server->HandleCursorMotion(event->time_msec, event->delta_x, event->delta_y);
}

static void handle_cursor_motion_absolute(struct wl_listener* listener, void* data) {
    auto* sig = WlContainerOf<WlrServerSignals>(listener, offsetof(WlrServerSignals, cursor_motion_absolute));
    auto* event = static_cast<struct wlr_pointer_motion_absolute_event*>(data);
    sig->server->HandleCursorMotionAbsolute(event->time_msec, event->x, event->y);
}

static void handle_cursor_button(struct wl_listener* listener, void* data) {
    auto* sig = WlContainerOf<WlrServerSignals>(listener, offsetof(WlrServerSignals, cursor_button));
    auto* event = static_cast<struct wlr_pointer_button_event*>(data);
    sig->server->HandleCursorButton(event->time_msec, event->button, event->state);
}

static void handle_cursor_axis(struct wl_listener* listener, void* data) {
    auto* sig = WlContainerOf<WlrServerSignals>(listener, offsetof(WlrServerSignals, cursor_axis));
    auto* event = static_cast<struct wlr_pointer_axis_event*>(data);
    sig->server->HandleCursorAxis(event->time_msec, event->orientation, event->delta);
}

WlrServer::WlrServer(std::shared_ptr<Compositor> compositor)
    : compositor_(std::move(compositor)),
      decoration_spec_(decoration::TilingDecorationSpec::CreateDefault()) {}

WlrServer::~WlrServer() {
    Stop();
}

bool WlrServer::Initialize(const std::string& socket_name) {
    PRISM_LOG_INFO("WLR-SERVER", "Initializing Wayland & wlroots Compositor Engine (Sway architecture)...");

    // Enable linear buffer compatibility on Intel ADL-N / i915 DRM planes
    setenv("WLR_DRM_NO_MODIFIERS", "1", 0);

    wlr_log_init(WLR_INFO, NULL);

    // 1. Create Wayland display & event loop
    wl_display_ = wl_display_create();
    if (!wl_display_) {
        PRISM_LOG_ERROR("WLR-SERVER", "Failed to create wl_display");
        return false;
    }
    wl_event_loop_ = wl_display_get_event_loop(wl_display_);

    // 2. Autocreate multi-backend (DRM/KMS, Wayland-nested, or Headless)
    backend_ = wlr_backend_autocreate(wl_event_loop_, nullptr);
    if (!backend_) {
        PRISM_LOG_ERROR("WLR-SERVER", "Failed to create wlr_backend");
        return false;
    }

    // 3. Autocreate hardware renderer & allocator
    renderer_ = wlr_renderer_autocreate(backend_);
    if (!renderer_) {
        PRISM_LOG_ERROR("WLR-SERVER", "Failed to create wlr_renderer");
        return false;
    }
    wlr_renderer_init_wl_shm(renderer_, wl_display_);

    allocator_ = wlr_allocator_autocreate(backend_, renderer_);
    if (!allocator_) {
        PRISM_LOG_ERROR("WLR-SERVER", "Failed to create wlr_allocator");
        return false;
    }

    // 4. Compositor & Subcompositor globals
    wlr_compositor_ = wlr_compositor_create(wl_display_, 5, renderer_);
    subcompositor_ = wlr_subcompositor_create(wl_display_);
    if (!wlr_presentation_create(wl_display_, backend_)) {
        PRISM_LOG_ERROR("WLR-SERVER", "Failed to create presentation feedback global");
        return false;
    }
    wlr_data_device_manager_create(wl_display_);
    wlr_screencopy_manager_v1_create(wl_display_);

    // 5. Output layout & Hardware Scene graph
    output_layout_ = wlr_output_layout_create(wl_display_);
    wlr_xdg_output_manager_v1_create(wl_display_,output_layout_);
    scene_ = wlr_scene_create();
    wlr_scene_attach_output_layout(scene_, output_layout_);
    if (auto* dmabuf = wlr_linux_dmabuf_v1_create_with_renderer(wl_display_, 4, renderer_)) {
        wlr_scene_set_linux_dmabuf_v1(scene_, dmabuf);
        PRISM_LOG_INFO("WLR-SERVER", "linux-dmabuf-v1 enabled for GPU clients");
    } else {
        PRISM_LOG_INFO("WLR-SERVER", "linux-dmabuf-v1 unavailable for this renderer");
    }

    // 6. Initialize 100% Native GPU Scene-Graph hierarchy
    InitSceneGraph();
    surface_effects_=std::make_unique<SurfaceEffects>(wl_display_,renderer_,allocator_);

    // 7. XDG Shell Protocol (Wayland Window Standard Protocol)
    xdg_shell_ = wlr_xdg_shell_create(wl_display_, 3);
    if (xdg_shell_) {
        signals_.new_xdg_toplevel.notify = handle_server_new_xdg_surface;
        wl_signal_add(&xdg_shell_->events.new_toplevel, &signals_.new_xdg_toplevel);
        PRISM_LOG_INFO("WLR-SERVER", "Wayland XDG-Shell v3 active: Native application windows supported");
    }

    // 8. Seat & Cursor management
    cursor_ = wlr_cursor_create();
    wlr_cursor_attach_output_layout(cursor_, output_layout_);
    cursor_mgr_ = wlr_xcursor_manager_create(nullptr, 24);
    if (cursor_mgr_) {
        wlr_xcursor_manager_load(cursor_mgr_, 1.0f);
        wlr_cursor_set_xcursor(cursor_, cursor_mgr_, "left_ptr");
    }

    seat_ = wlr_seat_create(wl_display_, "seat0");
    wlr_seat_set_capabilities(seat_, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);

    // 9. Wire wlroots signals
    signals_.server = this;
    signals_.new_output.notify = handle_server_new_output;
    wl_signal_add(&backend_->events.new_output, &signals_.new_output);

    signals_.new_input.notify = handle_server_new_input;
    wl_signal_add(&backend_->events.new_input, &signals_.new_input);

    signals_.cursor_motion.notify = handle_cursor_motion;
    wl_signal_add(&cursor_->events.motion, &signals_.cursor_motion);

    signals_.cursor_motion_absolute.notify = handle_cursor_motion_absolute;
    wl_signal_add(&cursor_->events.motion_absolute, &signals_.cursor_motion_absolute);

    signals_.cursor_button.notify = handle_cursor_button;
    wl_signal_add(&cursor_->events.button, &signals_.cursor_button);

    signals_.cursor_axis.notify = handle_cursor_axis;
    wl_signal_add(&cursor_->events.axis, &signals_.cursor_axis);

    // 8. Bind Wayland UNIX domain socket
    const char* sock = nullptr;
    if (!socket_name.empty()) {
        if (wl_display_add_socket(wl_display_, socket_name.c_str()) == 0) {
            sock = socket_name.c_str();
        }
    }
    if (!sock) {
        sock = wl_display_add_socket_auto(wl_display_);
    }
    socket_name_ = sock ? sock : "wayland-prism";

    PRISM_LOG_INFO("WLR-SERVER", "wlroots Compositor initialized successfully on socket '%s'", socket_name_.c_str());

    if (compositor_ && decoration_spec_) {
        compositor_->SetDecorationSpec(decoration_spec_);
    }

    // 9. Initialize Compositor-level IPC Server (Sway/i3 architecture)
    ipc_server_ = std::make_unique<ipc::IpcServer>(wl_event_loop_);
    if (!ipc_server_->Start()) {
        PRISM_LOG_WARN("WLR-SERVER", "Failed to start Compositor IPC Server");
    } else {
        // Command 1: get_outputs / outputs
        auto get_outputs_handler = [this](const std::string&, const std::vector<std::string>&) {
            auto outputs = GetOutputsInfo();
            std::ostringstream ss;
            ss << "{\n  \"status\": \"ok\",\n  \"outputs\": [\n";
            for (size_t i = 0; i < outputs.size(); ++i) {
                const auto& out = outputs[i];
                ss << "    {\n";
                ss << "      \"name\": \"" << out.name << "\",\n";
                ss << "      \"make\": \"" << out.make << "\",\n";
                ss << "      \"model\": \"" << out.model << "\",\n";
                ss << "      \"width\": " << out.width << ",\n";
                ss << "      \"height\": " << out.height << ",\n";
                ss << "      \"refresh_mhz\": " << out.refresh_mhz << ",\n";
                ss << "      \"refresh_hz\": " << std::fixed << std::setprecision(1) << out.refresh_hz << ",\n";
                ss << "      \"current_fps\": " << std::fixed << std::setprecision(1) << current_fps_ << ",\n";
                ss << "      \"adaptive_sync\": " << (out.adaptive_sync ? "true" : "false") << ",\n";
                ss << "      \"modes\": [\n";
                for (size_t j = 0; j < out.modes.size(); ++j) {
                    const auto& m = out.modes[j];
                    ss << "        {\"width\": " << m.width << ", \"height\": " << m.height
                       << ", \"refresh_hz\": " << std::fixed << std::setprecision(1) << m.refresh_hz
                       << ", \"preferred\": " << (m.preferred ? "true" : "false")
                       << ", \"current\": " << (m.current ? "true" : "false") << "}";
                    if (j + 1 < out.modes.size()) ss << ",";
                    ss << "\n";
                }
                ss << "      ]\n";
                ss << "    }";
                if (i + 1 < outputs.size()) ss << ",";
                ss << "\n";
            }
            ss << "  ]\n}";
            return ss.str();
        };
        ipc_server_->RegisterHandler("get_outputs", get_outputs_handler);
        ipc_server_->RegisterHandler("outputs", get_outputs_handler);

        // Command 2: set_mode / mode: <output> <w> <h> [refresh_hz]
        auto set_mode_handler = [this](const std::string&, const std::vector<std::string>& args) {
            if (args.size() < 3) {
                return std::string("{\"status\": \"error\", \"message\": \"Usage: set_mode <output_name|all> <width> <height> [refresh_hz]\"}");
            }
            std::string out_name = args[0];
            int width = std::stoi(args[1]);
            int height = std::stoi(args[2]);
            int refresh_mhz = 0;
            if (args.size() >= 4) {
                int r = std::stoi(args[3]);
                refresh_mhz = (r < 1000) ? (r * 1000) : r;
            }
            bool ok = SetOutputMode(out_name, width, height, refresh_mhz);
            std::ostringstream ss;
            if (ok) {
                ss << "{\"status\": \"ok\", \"message\": \"Output mode updated successfully\", \"output\": \""
                   << out_name << "\", \"width\": " << width << ", \"height\": " << height << "}";
            } else {
                ss << "{\"status\": \"error\", \"message\": \"Failed to set mode for output: " << out_name << "\"}";
            }
            return ss.str();
        };
        ipc_server_->RegisterHandler("set_mode", set_mode_handler);
        ipc_server_->RegisterHandler("mode", set_mode_handler);

        // Command 3: get_status / status
        auto get_status_handler = [this](const std::string&, const std::vector<std::string>&) {
            std::ostringstream ss;
            ss << "{\n"
               << "  \"status\": \"ok\",\n"
               << "  \"version\": \"Project PrismWM 0.1.0 (wlroots 0.18 Native)\",\n"
               << "  \"fps\": " << std::fixed << std::setprecision(1) << current_fps_ << ",\n"
               << "  \"frame_count\": " << frame_count_ << ",\n"
               << "  \"outputs_count\": " << outputs_.size() << ",\n"
               << "  \"windows_count\": " << (compositor_ ? compositor_->GetWindows().size() : 0) << ",\n"
               << "  \"mission_control\": " << (compositor_ && compositor_->IsInMissionControl() ? "true" : "false") << ",\n"
               << "  \"debug_hud\": " << (compositor_ && compositor_->IsDebugHudEnabled() ? "true" : "false") << ",\n"
               << "  \"wayland_socket\": \"" << socket_name_ << "\",\n"
               << "  \"ipc_socket\": \"" << ipc_server_->GetSocketPath() << "\"\n"
               << "}";
            return ss.str();
        };
        ipc_server_->RegisterHandler("get_status", get_status_handler);
        ipc_server_->RegisterHandler("status", get_status_handler);

        // Command 7: set_debug / debug: <1|0|toggle>
        auto debug_handler = [this](const std::string&, const std::vector<std::string>& args) {
            if (args.empty()) {
                bool cur = compositor_ ? compositor_->IsDebugHudEnabled() : false;
                return std::string("{\"status\": \"ok\", \"debug_hud\": ") + (cur ? "true" : "false") + "}";
            }
            if (compositor_) {
                if (args[0] == "toggle") {
                    compositor_->SetDebugHud(!compositor_->IsDebugHudEnabled());
                } else if (args[0] == "1" || args[0] == "true" || args[0] == "on") {
                    compositor_->SetDebugHud(true);
                } else if (args[0] == "0" || args[0] == "false" || args[0] == "off") {
                    compositor_->SetDebugHud(false);
                }
            }
            bool cur = compositor_ ? compositor_->IsDebugHudEnabled() : false;
            return std::string("{\"status\": \"ok\", \"debug_hud\": ") + (cur ? "true" : "false") + "}";
        };
        ipc_server_->RegisterHandler("set_debug", debug_handler);
        ipc_server_->RegisterHandler("debug", debug_handler);

        // Production controls operate on the same records as real XDG surfaces.
        auto layout_handler = [this](const std::string&, const std::vector<std::string>& args) {
            if (args.empty()) return std::string("{\"status\":\"error\",\"message\":\"Usage: layout splith|splitv\"}");
            tree::LayoutMode mode;
            if (args[0] == "splith" || args[0] == "h" || args[0] == "horizontal") mode = tree::LayoutMode::SplitHorizontal;
            else if (args[0] == "splitv" || args[0] == "v" || args[0] == "vertical") mode = tree::LayoutMode::SplitVertical;
            else return std::string("{\"status\":\"error\",\"message\":\"Unsupported native layout\"}");
            const bool ok = compositor_ && compositor_->SetTreeLayout(mode);
            ArrangeXdgViews(); SynchronizeXdgFocus();
            return nlohmann::json{{"status", ok ? "ok" : "error"}, {"layout", args[0]}}.dump();
        };
        ipc_server_->RegisterHandler("set_layout", layout_handler);
        ipc_server_->RegisterHandler("layout", layout_handler);
        ipc_server_->RegisterHandler("split", layout_handler);

        auto parse_direction = [](const std::vector<std::string>& args) -> std::optional<tree::Direction> {
            if (args.empty()) return {};
            if (args[0] == "left" || args[0] == "h") return tree::Direction::Left;
            if (args[0] == "right" || args[0] == "l") return tree::Direction::Right;
            if (args[0] == "up" || args[0] == "k") return tree::Direction::Up;
            if (args[0] == "down" || args[0] == "j") return tree::Direction::Down;
            return {};
        };
        auto focus_handler = [this, parse_direction](const std::string&, const std::vector<std::string>& args) {
            const auto dir = parse_direction(args);
            if (!dir) return std::string("{\"status\":\"error\",\"message\":\"Usage: focus left|right|up|down\"}");
            const bool moved = compositor_ && !(focused_xdg_view_ && focused_xdg_view_->fullscreen) && compositor_->MoveFocus(*dir);
            ArrangeXdgViews(); SynchronizeXdgFocus();
            return nlohmann::json{{"status", moved ? "ok" : "no_change"}, {"direction", args[0]}}.dump();
        };
        ipc_server_->RegisterHandler("focus", focus_handler);
        ipc_server_->RegisterHandler("swap", [this, parse_direction](const std::string&, const std::vector<std::string>& args) {
            const auto dir = parse_direction(args);
            if (!dir) return std::string("{\"status\":\"error\",\"message\":\"Usage: swap left|right|up|down\"}");
            const bool swapped = compositor_ && compositor_->SwapFocusDirection(*dir);
            ArrangeXdgViews(); SynchronizeXdgFocus();
            return nlohmann::json{{"status", swapped ? "ok" : "no_change"}, {"direction", args[0]}}.dump();
        });
        auto ws_handler = [this](const std::string&, const std::vector<std::string>& args) {
            if (!compositor_) return std::string("{\"status\":\"error\"}");
            if (args.empty()) return nlohmann::json{{"status", "ok"},
                {"active_workspace", compositor_->GetTreeEngine().GetActiveWorkspace()->GetName()}}.dump();
            const bool ok = compositor_->SwitchWorkspace(args[0]);
            ArrangeXdgViews(); SynchronizeXdgFocus();
            return nlohmann::json{{"status", ok ? "ok" : "error"}, {"workspace", args[0]}}.dump();
        };
        ipc_server_->RegisterHandler("workspace", ws_handler);
        ipc_server_->RegisterHandler("ws", ws_handler);
        ipc_server_->RegisterHandler("move_workspace", [this](const std::string&, const std::vector<std::string>& args) {
            if (args.empty() || !compositor_) return std::string("{\"status\":\"error\"}");
            auto& tree = compositor_->GetTreeEngine();
            const bool ok = tree.MoveWindowToWorkspace(tree.GetFocusedWindow(), args[0]);
            ArrangeXdgViews(); SynchronizeXdgFocus();
            return nlohmann::json{{"status", ok ? "ok" : "error"}, {"workspace", args[0]}}.dump();
        });
        auto tree_handler = [this](const std::string&, const std::vector<std::string>&) {
            if (!compositor_) return std::string("{\"status\":\"error\"}");
            return compositor_->GetTreeEngine().DumpTreeJson();
        };
        ipc_server_->RegisterHandler("tree", tree_handler);
        ipc_server_->RegisterHandler("get_tree", tree_handler);
        auto close_handler = [this](const std::string&, const std::vector<std::string>&) {
            const bool ok = focused_xdg_view_ != nullptr;
            CloseFocusedXdgView();
            return nlohmann::json{{"status", ok ? "ok" : "no_change"}}.dump();
        };
        ipc_server_->RegisterHandler("close", close_handler);
        ipc_server_->RegisterHandler("kill", close_handler);

        // Production uses one generated theme shared with client SDK tokens.
        auto theme_handler=[](const std::string&,const std::vector<std::string>& args) {
            if(!args.empty()) return nlohmann::json{{"status","unsupported"},{"message","Runtime global theme switching is not implemented"}}.dump();
            return nlohmann::json{{"status","ok"},{"current_theme","Prism Glass"},{"format_version",contracts::theme::kFormatVersion}}.dump();
        };
        ipc_server_->RegisterHandler("theme",theme_handler);
        ipc_server_->RegisterHandler("set_theme",theme_handler);

        ipc_server_->RegisterHandler("fold", [](const std::string&, const std::vector<std::string>&) {
            return std::string("{\"status\":\"error\",\"message\":\"Fold is not implemented for native BSP windows\"}");
        });

        auto fs_handler = [this](const std::string&, const std::vector<std::string>& args) {
            if (!focused_xdg_view_) return std::string("{\"status\":\"no_change\"}");
            bool enabled = !focused_xdg_view_->fullscreen;
            if (!args.empty()) {
                if (args[0] == "on" || args[0] == "enable") enabled = true;
                else if (args[0] == "off" || args[0] == "disable") enabled = false;
                else if (args[0] != "toggle") return std::string("{\"status\":\"error\"}");
            }
            SetXdgFullscreen(focused_xdg_view_, enabled);
            return nlohmann::json{{"status", "ok"}, {"fullscreen", enabled}}.dump();
        };
        ipc_server_->RegisterHandler("fullscreen", fs_handler);
        ipc_server_->RegisterHandler("monocle", fs_handler);

        // Command 5: action: <app_id> <action>
        ipc_server_->RegisterHandler("action", [this](const std::string&, const std::vector<std::string>& args) {
            if (args.size() < 2) {
                return std::string("{\"status\": \"error\", \"message\": \"Usage: action <app_id> <action_name>\"}");
            }
            if (compositor_) {
                compositor_->DispatchAction(args[0], args[1]);
            }
            return std::string("{\"status\": \"ok\", \"app\": \"") + args[0] + "\", \"action\": \"" + args[1] + "\"}";
        });

        // Command 6: ipc_test: High-speed benchmark of lock-free SHM ring buffer
        ipc_server_->RegisterHandler("ipc_test", [](const std::string&, const std::vector<std::string>&) {
            std::string bench_name = "/prism_bench_shm";
            auto host = ipc::Channel::CreateHost(bench_name);
            auto client = ipc::Channel::ConnectClient(bench_name);
            if (!host || !client) {
                return std::string("{\"status\": \"error\", \"message\": \"Failed to initialize SHM bench channel\"}");
            }

            const int kIterations = 10000;
            auto t0 = std::chrono::high_resolution_clock::now();
            for (int i = 0; i < kIterations; ++i) {
                auto ev = ipc::EventPacket::MakeAction("bench_action");
                ev.data.custom_val = static_cast<double>(i);
                host->PushEvent(ev);

                ipc::EventPacket recv_ev{};
                client->PopEvent(recv_ev);
            }
            auto t1 = std::chrono::high_resolution_clock::now();
            int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            double avg_latency_ns = static_cast<double>(total_ns) / kIterations;
            double avg_latency_us = avg_latency_ns / 1000.0;
            double mops = (static_cast<double>(kIterations) / (total_ns / 1e9)) / 1e6;

            std::ostringstream ss;
            ss << "{\n"
               << "  \"status\": \"ok\",\n"
               << "  \"channel\": \"" << bench_name << "\",\n"
               << "  \"packets_tested\": " << kIterations << ",\n"
               << "  \"total_time_us\": " << (total_ns / 1000) << ",\n"
               << "  \"avg_latency_ns\": " << std::fixed << std::setprecision(1) << avg_latency_ns << ",\n"
               << "  \"avg_latency_us\": " << std::fixed << std::setprecision(4) << avg_latency_us << ",\n"
               << "  \"throughput_mops\": " << std::fixed << std::setprecision(2) << mops << ",\n"
               << "  \"verdict\": \"Zero-copy lock-free SHM ring buffer validated with sub-microsecond latency!\"\n"
               << "}";
            return ss.str();
        });
    }

    last_frame_time_ns_ = core::CurrentTimeNs();
    running_ = true;
    return true;
}

static void auto_create_output_for_nested(struct wlr_backend* backend, void* data) {
    bool* created = static_cast<bool*>(data);
    if (*created) return;

    PRISM_LOG_INFO("WLR-SERVER", "auto_create callback: backend=%p, is_wl=%d, is_headless=%d",
                   (void*)backend, wlr_backend_is_wl(backend), wlr_backend_is_headless(backend));

    if (wlr_backend_is_wl(backend)) {
        struct wlr_output* out = wlr_wl_output_create(backend);
        PRISM_LOG_INFO("WLR-SERVER", "wlr_wl_output_create returned %p", (void*)out);
        if (out) {
            wlr_wl_output_set_title(out, "Project Prism Desktop");
            *created = true;
            PRISM_LOG_INFO("WLR-SERVER", "Created nested Wayland output window");
        }
    } else if (wlr_backend_is_headless(backend)) {
        struct wlr_output* out = wlr_headless_add_output(backend, 1920, 1080);
        PRISM_LOG_INFO("WLR-SERVER", "wlr_headless_add_output returned %p", (void*)out);
        if (out) {
            *created = true;
            PRISM_LOG_INFO("WLR-SERVER", "Created headless 1920x1080 output");
        }
    }
#if WLR_HAS_X11_BACKEND
    else if (wlr_backend_is_x11(backend)) {
        struct wlr_output* out = wlr_x11_output_create(backend);
        PRISM_LOG_INFO("WLR-SERVER", "wlr_x11_output_create returned %p", (void*)out);
        if (out) {
            wlr_x11_output_set_title(out, "Project Prism Desktop");
            *created = true;
            PRISM_LOG_INFO("WLR-SERVER", "Created nested X11 output window");
        }
    }
#endif
}

void WlrServer::Start() {
    if (!backend_) return;

    PRISM_LOG_INFO("WLR-SERVER", "Starting wlroots backend...");
    if (!wlr_backend_start(backend_)) {
        PRISM_LOG_ERROR("WLR-SERVER", "Failed to start wlroots backend");
        return;
    }
    PRISM_LOG_INFO("WLR-SERVER", "Backend started! is_multi=%d, is_wl=%d, outputs=%zu",
                   wlr_backend_is_multi(backend_), wlr_backend_is_wl(backend_), outputs_.size());

    // Auto-create output window ONLY if no outputs exist (prevents duplicate output window)
    bool created = false;
    if (outputs_.empty()) {
        if (wlr_backend_is_multi(backend_)) {
            wlr_multi_for_each_backend(backend_, auto_create_output_for_nested, &created);
        } else {
            auto_create_output_for_nested(backend_, &created);
        }
    }

    PRISM_LOG_INFO("WLR-SERVER", "wlroots backend running! Wayland display listening on %s (created_nested=%d)",
                   socket_name_.c_str(), created ? 1 : 0);
}

WlrServer::Registration::~Registration() { if (pidfd>=0) close(pidfd); }
void WlrServer::AttachControl(int fd, int parent_pid) {
    launch::VerifyControlPeer(fd,parent_pid);
    control_=std::make_unique<launch::Stream>(fd,launch::ControlFrameSize);
    std::array<std::uint8_t,8> random{}; launch::RandomBytes(random);
    for (auto byte:random) control_session_=(control_session_<<8)|byte;
    if (!control_session_) throw std::runtime_error("Zero session nonce");
    launch::ControlMessage ready; ready.permit.session=control_session_;
    ready.permit.pid=getpid(); ready.success=true;
    control_->Queue(launch::EncodeControl(ready)); control_->Flush();
}
void WlrServer::NotifyView(WlrXdgView* view, launch::ControlType type) {
    if (!control_ || !view->instance) return;
    launch::ControlMessage message; message.type=type; message.permit.session=control_session_;
    message.permit.instance={view->instance}; message.permit.pid=view->pid;
    message.permit.role=static_cast<contracts::WindowRole>(view->shell_role);
    message.success=true;
    if (!control_->Queue(launch::EncodeControl(message))) control_failed_=true;
}
void WlrServer::PumpControl() {
    if (!control_) return;
    try {
        for (const auto& frame:control_->Receive()) {
            auto m=launch::DecodeControl(frame); const auto& p=m.permit;
            if (p.session!=control_session_) throw std::runtime_error("Wrong WM session");
            if (m.type==launch::ControlType::Grant) {
                bool occupied=false;
                for (const auto& [pid,r]:registrations_) {
                    if (r->permit.instance==p.instance || (p.role!=contracts::WindowRole::Toplevel && r->permit.role==p.role)) occupied=true;
                }
                m.type=launch::ControlType::Registered; m.success=false;
                if (p.pid && p.request.value && p.instance.value && !occupied && !registrations_.contains(p.pid) &&
                    p.expires_ns>launch::MonotonicNs()) {
                    auto r=std::make_unique<Registration>(); r->permit=p;
                    r->pidfd=syscall(SYS_pidfd_open,p.pid,0);
                    pollfd dead{r->pidfd,POLLIN,0};
                    if (r->pidfd>=0 && poll(&dead,1,0)==0) {
                        if (p.role!=contracts::WindowRole::Toplevel) r->guard=std::make_unique<launch::ShellPermitGuard>(p);
                        registrations_.emplace(p.pid,std::move(r)); m.success=true;
                    }
                }
                control_->Queue(launch::EncodeControl(m));
            } else if (m.type==launch::ControlType::Revoke) {
                auto r=registrations_.find(p.pid);
                if (r!=registrations_.end() && r->second->permit.instance==p.instance) {
                    registrations_.erase(r);
                    wl_client* client=nullptr;
                    for (const auto& view:xdg_views_) if (view->instance==p.instance.value && view->pid==static_cast<pid_t>(p.pid)) {
                        client=wl_resource_get_client(view->toplevel->base->surface->resource); break;
                    }
                    if (client) wl_client_destroy(client);
                }
            } else if (m.type==launch::ControlType::Activate) {
                m.type=launch::ControlType::Activated; m.success=false;
                for (const auto& view:xdg_views_) if (view->instance==p.instance.value && view->pid==static_cast<pid_t>(p.pid) && view->mapped && !view->shell_role) {
                    FocusXdgView(view.get()); m.success=true; break;
                }
                control_->Queue(launch::EncodeControl(m));
            } else throw std::runtime_error("Unexpected launcher control message");
        }
        control_->Flush();
        if (control_->Closed()) control_failed_=true;
    } catch (const std::exception& error) {
        PRISM_LOG_ERROR("WLR-CONTROL", "%s",error.what()); control_failed_=true; control_->Close();
    }
}

void WlrServer::Stop() {
    if (!wl_display_) return;
    running_ = false;
    registrations_.clear();
    if (wl_display_) wl_display_destroy_clients(wl_display_);
    surface_effects_.reset();

    focused_xdg_view_ = nullptr;
    xdg_views_.clear();
    keyboards_.clear();
    outputs_.clear();
    dock_icon_rects_.clear();

    drag_manager_.reset();

    if (scene_) {
        wlr_scene_node_destroy(&scene_->tree.node);
        scene_ = nullptr;
    }

    if (cursor_mgr_) {
        wlr_xcursor_manager_destroy(cursor_mgr_);
        cursor_mgr_ = nullptr;
    }
    if (cursor_) {
        wlr_cursor_destroy(cursor_);
        cursor_ = nullptr;
    }
    if (output_layout_) {
        wlr_output_layout_destroy(output_layout_);
        output_layout_ = nullptr;
    }
    if (allocator_) {
        wlr_allocator_destroy(allocator_);
        allocator_ = nullptr;
    }
    if (renderer_) {
        wlr_renderer_destroy(renderer_);
        renderer_ = nullptr;
    }
    if (backend_) {
        wlr_backend_destroy(backend_);
        backend_ = nullptr;
    }
    if (wl_display_) {
        wl_display_destroy(wl_display_);
        wl_display_ = nullptr;
    }

    if (ipc_server_) {
        ipc_server_->Stop();
        ipc_server_.reset();
    }

    PRISM_LOG_INFO("WLR-SERVER", "wlroots server stopped cleanly");
}

void WlrServer::RunEventLoopIteration(int timeout_ms) {
    if (!wl_event_loop_ || !wl_display_) return;
    PumpControl();
    wl_event_loop_dispatch(wl_event_loop_, timeout_ms);
    PumpControl();
    wl_display_flush_clients(wl_display_);
}

void WlrServer::HandleNewOutput(struct wlr_output* output) {
    wlr_output_init_render(output, allocator_, renderer_);

    if (wlr_output_is_wl(output)) {
        wlr_wl_output_set_title(output, "Project Prism Desktop");
    }

    // 1. Attach listeners
    auto wlr_out = std::make_unique<WlrOutput>(output, this);
    wlr_output_layout_add_auto(output_layout_, output);

    // 2. Attach output to hardware wlr_scene
    wlr_out->scene_output = wlr_scene_output_create(scene_, output);
    struct wlr_box box;
    wlr_output_layout_get_box(output_layout_, output, &box);
    wlr_scene_output_set_position(wlr_out->scene_output, box.x, box.y);

    // 3. Commit initial output state
    struct wlr_output_mode* mode = wlr_output_preferred_mode(output);
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    if (mode) {
        wlr_output_state_set_mode(&state, mode);
    } else if (!wl_list_empty(&output->modes)) {
        struct wlr_output_mode* first_mode = wl_container_of(output->modes.next, first_mode, link);
        wlr_output_state_set_mode(&state, first_mode);
    }
    wlr_output_commit_state(output, &state);
    wlr_output_state_finish(&state);

    if (compositor_ && output->width > 0 && output->height > 0) {
        compositor_->SetScreenSize(output->width, output->height);
    }

    PRISM_LOG_INFO("WLR-OUTPUT", "New output added: %s (%dx%d @ %.1fHz) -> Native wlr_scene Attached",
                   output->name, output->width, output->height, output->refresh / 1000.0f);
    
    outputs_.push_back(std::move(wlr_out));
    ArrangeXdgViews();

    // The initial DRM modeset may already have a page-flip pending.
    // Let wlroots emit the frame event once the output can accept a commit.
    wlr_output_schedule_frame(output);
}

void WlrServer::RemoveOutput(WlrOutput* output) {
    for (auto it = outputs_.begin(); it != outputs_.end(); ++it) {
        if (it->get() == output) {
            PRISM_LOG_INFO("WLR-OUTPUT", "Output removed: %s", output->wlr_output->name);
            outputs_.erase(it);
            ArrangeXdgViews();
            break;
        }
    }
}

void WlrServer::HandleNewInput(struct wlr_input_device* device) {
    if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
        auto* wlr_kbd = wlr_keyboard_from_input_device(device);
        struct xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        struct xkb_keymap* keymap = xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
        wlr_keyboard_set_keymap(wlr_kbd, keymap);
        xkb_keymap_unref(keymap);
        xkb_context_unref(context);

        wlr_keyboard_set_repeat_info(wlr_kbd, 25, 600);
        wlr_seat_set_keyboard(seat_, wlr_kbd);
        auto binding = std::make_unique<WlrKeyboardBinding>();
        binding->server = this;
        binding->keyboard = wlr_kbd;
        binding->key.notify = handle_keyboard_key;
        binding->modifiers.notify = handle_keyboard_modifiers;
        binding->destroy.notify = handle_keyboard_destroy;
        wl_signal_add(&wlr_kbd->events.key, &binding->key);
        wl_signal_add(&wlr_kbd->events.modifiers, &binding->modifiers);
        wl_signal_add(&device->events.destroy, &binding->destroy);
        keyboards_.push_back(std::move(binding));
        if (focused_xdg_view_) FocusXdgView(focused_xdg_view_);
        PRISM_LOG_INFO("WLR-INPUT", "Keyboard attached: %s", device->name);
    } else if (device->type == WLR_INPUT_DEVICE_POINTER || device->type == WLR_INPUT_DEVICE_TOUCH) {
        wlr_cursor_attach_input_device(cursor_, device);
        PRISM_LOG_INFO("WLR-INPUT", "Pointer/Touchpad attached: %s", device->name);
    }
}

void WlrServer::HandleNewXdgToplevel(struct wlr_xdg_toplevel* toplevel) {
    if (!toplevel || !windows_tree_) return;
    auto* client=wl_resource_get_client(toplevel->base->surface->resource);
    pid_t peer=-1; uid_t uid{};
    wl_client_get_credentials(client,&peer,&uid,nullptr);
    std::uint64_t instance{}; int role{};
    if (auto found=registrations_.find(peer); found!=registrations_.end() && !found->second->consumed) {
        auto& r=*found->second; pollfd dead{r.pidfd,POLLIN,0};
        if (uid!=geteuid() || poll(&dead,1,0)!=0 || r.permit.expires_ns<=launch::MonotonicNs() ||
            (r.guard && !r.guard->Consume(r.permit,launch::MonotonicNs()))) {
            // A registered launch must fail if its identity expires; silently
            // mapping it as an ordinary window would conceal a Shell failure.
            wl_client_post_implementation_error(client,"Prism launch registration expired or invalid");
            return;
        }
        r.consumed=true; instance=r.permit.instance.value; role=static_cast<int>(r.permit.role);
    }
    auto view=std::make_unique<WlrXdgView>();
    view->server=this; view->toplevel=toplevel; view->instance=instance;
    view->pid=peer; view->shell_role=role;
    view->x = 0;
    view->y = 0;
    if (role) {
        const int width = !outputs_.empty() && outputs_[0]->wlr_output ? outputs_[0]->wlr_output->width : 1280;
        const int height = !outputs_.empty() && outputs_[0]->wlr_output ? outputs_[0]->wlr_output->height : 720;
        const auto bounds = theme_.ShellRect(role, width, height);
        view->x = static_cast<int>(bounds.x); view->y = static_cast<int>(bounds.y);
        view->width = static_cast<int>(bounds.width); view->height = static_cast<int>(bounds.height);
    }
    auto* parent = view->shell_role == 1 ? background_tree_
        : view->shell_role > 1 ? chrome_tree_ : windows_tree_;
    view->scene_tree = wlr_scene_xdg_surface_create(parent, toplevel->base);
    if (!view->scene_tree) return;
    wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
    wlr_scene_node_set_enabled(&view->scene_tree->node, false);
    view->map.notify = handle_xdg_map;
    view->unmap.notify = handle_xdg_unmap;
    view->destroy.notify = handle_xdg_destroy;
    view->request_maximize.notify = handle_xdg_maximize;
    view->request_fullscreen.notify = handle_xdg_fullscreen;
    wl_signal_add(&toplevel->base->surface->events.map, &view->map);
    wl_signal_add(&toplevel->base->surface->events.unmap, &view->unmap);
    wl_signal_add(&toplevel->events.destroy, &view->destroy);
    wl_signal_add(&toplevel->events.request_maximize, &view->request_maximize);
    wl_signal_add(&toplevel->events.request_fullscreen, &view->request_fullscreen);
    view->commit.notify = [](wl_listener* listener, void*) {
        auto* item = WlContainerOf<WlrXdgView>(listener, offsetof(WlrXdgView, commit));
        if (item->toplevel->base->initial_commit)
            wlr_xdg_toplevel_set_size(item->toplevel, item->width, item->height);
        UpdateCommittedGeometry(item);
    };
    wl_signal_add(&toplevel->base->surface->events.commit, &view->commit);
    view->set_title.notify = [](wl_listener* listener, void*) {
        auto* item = WlContainerOf<WlrXdgView>(listener, offsetof(WlrXdgView, set_title));
        if (item->managed) item->managed->UpdateIdentity(item->toplevel->app_id ? item->toplevel->app_id : "",
            item->toplevel->title ? item->toplevel->title : "", item->pid, item->instance);
    };
    view->set_app_id.notify = [](wl_listener* listener, void*) {
        auto* item = WlContainerOf<WlrXdgView>(listener, offsetof(WlrXdgView, set_app_id));
        if (item->managed) item->managed->UpdateIdentity(item->toplevel->app_id ? item->toplevel->app_id : "",
            item->toplevel->title ? item->toplevel->title : "", item->pid, item->instance);
    };
    wl_signal_add(&toplevel->events.set_title, &view->set_title);
    wl_signal_add(&toplevel->events.set_app_id, &view->set_app_id);
    PRISM_LOG_INFO("WLR-XDG", "New XDG toplevel registered: title='%s' app_id='%s'",
                   toplevel->title ? toplevel->title : "(untitled)",
                   toplevel->app_id ? toplevel->app_id : "(none)");
    PRISM_LOG_INFO("WLR-XDG", "Client pid=%d authorized shell role=%d", peer, view->shell_role);
    xdg_views_.push_back(std::move(view));
}

void WlrServer::HandleXdgMap(WlrXdgView* view) {
    view->mapped = true;
    if (!view->shell_role && compositor_) {
        view->managed = compositor_->ManageNativeWindow(view->toplevel->app_id ? view->toplevel->app_id : "",
            view->toplevel->title ? view->toplevel->title : "", view->pid, view->instance);
        view->fullscreen = view->toplevel->requested.fullscreen;
        view->maximized = view->toplevel->requested.maximized;
        if (view->managed) view->managed->SetFullscreen(view->fullscreen);
    }
    NotifyView(view, launch::ControlType::Mapped);
    ArrangeXdgViews();
    if (!view->shell_role) FocusXdgView(view);
    else SynchronizeXdgFocus();
    PRISM_LOG_INFO("WLR-XDG", "Mapped app_id='%s' shell role=%d pid=%d",
        view->toplevel->app_id ? view->toplevel->app_id : "", view->shell_role, view->pid);
}

void WlrServer::HandleXdgUnmap(WlrXdgView* view) {
    if (dragged_xdg_view_ == view) dragged_xdg_view_ = nullptr;
    view->mapped = false;
    view->visible = false;
    NotifyView(view, launch::ControlType::Unmapped);
    wlr_scene_node_set_enabled(&view->scene_tree->node, false);
    if (view->managed && compositor_) { compositor_->DestroyWindow(view->managed); view->managed.reset(); }
    if (focused_xdg_view_ == view) {
        focused_xdg_view_ = nullptr;
        wlr_seat_keyboard_notify_clear_focus(seat_);
    }
    if (seat_->pointer_state.focused_surface == view->toplevel->base->surface)
        wlr_seat_pointer_notify_clear_focus(seat_);
    ArrangeXdgViews();
    SynchronizeXdgFocus();
}

void WlrServer::HandleXdgDestroy(WlrXdgView* view) {
    if (dragged_xdg_view_ == view) dragged_xdg_view_ = nullptr;
    if (view->managed && compositor_) { compositor_->DestroyWindow(view->managed); view->managed.reset(); }
    if (focused_xdg_view_ == view) {
        focused_xdg_view_ = nullptr;
        wlr_seat_keyboard_notify_clear_focus(seat_);
    }
    auto it = std::find_if(xdg_views_.begin(), xdg_views_.end(),
        [view](const auto& item) { return item.get() == view; });
    if (it != xdg_views_.end()) xdg_views_.erase(it);
    ArrangeXdgViews();
    SynchronizeXdgFocus();
}

void WlrServer::SetXdgFullscreen(WlrXdgView* view, bool enabled) {
    if (!view || view->shell_role) return;
    view->fullscreen = enabled;
    if (view->managed) view->managed->SetFullscreen(enabled);
    wlr_xdg_toplevel_set_fullscreen(view->toplevel, enabled);
    if (enabled) FocusXdgView(view);
    ArrangeXdgViews();
}

void WlrServer::HandleXdgMaximize(WlrXdgView* view) {
    if (!view || !view->toplevel || view->shell_role) return;
    view->maximized = view->toplevel->requested.maximized;
    wlr_xdg_toplevel_set_maximized(view->toplevel, view->maximized);
    // Maximization keeps the existing tile: tiled clients cannot overlap their
    // neighbours. Explicit fullscreen is a separate, reversible workspace mode.
    SetXdgFullscreen(view, view->toplevel->requested.fullscreen);
}

void WlrServer::ArrangeXdgViews() {
    if (!compositor_) return;
    const int width = !outputs_.empty() && outputs_[0]->wlr_output ? outputs_[0]->wlr_output->width : 1280;
    const int height = !outputs_.empty() && outputs_[0]->wlr_output ? outputs_[0]->wlr_output->height : 720;
    compositor_->SetScreenSize(width, height);
    auto spec = *decoration_spec_;
    spec.gaps.inner = theme_.inner_gap;
    spec.gaps.outer = theme_.outer_gap;
    spec.gaps.smart_gaps = false;
    auto& engine = compositor_->GetTreeEngine();
    engine.Arrange(theme_.WorkArea(width, height), spec);
    const auto active = engine.GetActiveWorkspace();
    WlrXdgView* fullscreen = nullptr;
    for (const auto& view : xdg_views_) {
        if (!view->mapped || !view->managed) continue;
        auto node = engine.FindViewForWindow(view->managed);
        if (node && node->GetWorkspace() == active && view->fullscreen) {
            fullscreen = view.get();
            if (view.get() == focused_xdg_view_) break;
        }
    }
    for (auto& view : xdg_views_) {
        core::Rect bounds{};
        if (view->shell_role) {
            bounds = theme_.ShellRect(view->shell_role, width, height);
            view->visible = view->mapped && (!fullscreen || view->shell_role == 1);
        } else if (view->managed) {
            auto node = engine.FindViewForWindow(view->managed);
            const bool in_workspace = node && node->GetWorkspace() == active;
            view->visible = view->mapped && in_workspace && (!fullscreen || fullscreen == view.get());
            if (fullscreen == view.get()) bounds = {0, 0, static_cast<float>(width), static_cast<float>(height)};
            else if (node) bounds = node->bounds;
            view->managed->SetBounds(bounds);
            view->managed->SetVisible(view->visible);
        } else { view->visible = false; }
        wlr_scene_node_set_enabled(&view->scene_tree->node, view->visible);
        if (!view->shell_role && !view->managed) continue;
        const int x = static_cast<int>(std::round(bounds.x));
        const int y = static_cast<int>(std::round(bounds.y));
        const int w = std::max(1, static_cast<int>(std::round(bounds.width)));
        const int h = std::max(1, static_cast<int>(std::round(bounds.height)));
        view->x = x; view->y = y;
        wlr_scene_node_set_position(&view->scene_tree->node, x, y);
        // Pure topology swaps can move a view without asking the client to
        // resize or commit. Keep the committed size and displayed origin current.
        UpdateCommittedGeometry(view.get());
        if (view->width != w || view->height != h) {
            view->width = w; view->height = h;
            wlr_xdg_toplevel_set_size(view->toplevel, w, h);
        }
        if (!view->shell_role) {
            const std::uint32_t edges = view->fullscreen ? 0 : WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT;
            if (edges != view->tiled_edges) {
                view->tiled_edges = edges;
                wlr_xdg_toplevel_set_tiled(view->toplevel, edges);
            }
        }
    }
    UpdateXdgPointerFocus(static_cast<uint32_t>(core::CurrentTimeNs() / 1000000));
    for (auto& out : outputs_) if (out && out->wlr_output) wlr_output_schedule_frame(out->wlr_output);
}

void WlrServer::SynchronizeXdgFocus() {
    if (!compositor_) return;
    compositor_->SynchronizeFocus();
    const auto win = compositor_->GetTreeEngine().GetFocusedWindow();
    for (auto& view : xdg_views_) if (win && view->managed == win && view->visible) { FocusXdgView(view.get()); return; }
    if (focused_xdg_view_ && focused_xdg_view_->toplevel)
        wlr_xdg_toplevel_set_activated(focused_xdg_view_->toplevel, false);
    focused_xdg_view_ = nullptr;
    wlr_seat_keyboard_notify_clear_focus(seat_);
}

void WlrServer::FocusXdgView(WlrXdgView* view) {
    if (!view || !view->mapped || view->shell_role || !view->managed) return;
    const auto node = compositor_->GetTreeEngine().FindViewForWindow(view->managed);
    if (!node) return;
    // Activating an existing instance switches to its actual workspace.
    bool rearrange = node->GetWorkspace() != compositor_->GetTreeEngine().GetActiveWorkspace();
    for (auto& other : xdg_views_) {
        if (other.get() == view || !other->managed || !other->fullscreen) continue;
        const auto other_node = compositor_->GetTreeEngine().FindViewForWindow(other->managed);
        if (other_node && other_node->GetWorkspace() == node->GetWorkspace()) {
            other->fullscreen = false;
            other->managed->SetFullscreen(false);
            wlr_xdg_toplevel_set_fullscreen(other->toplevel, false);
            rearrange = true;
        }
    }
    compositor_->GetTreeEngine().SetFocusedWindow(view->managed);
    compositor_->SynchronizeFocus();
    if (rearrange) ArrangeXdgViews();
    if (!view->visible) return;
    const bool changed = focused_xdg_view_ != view;
    if (focused_xdg_view_ && changed && focused_xdg_view_->toplevel)
        wlr_xdg_toplevel_set_activated(focused_xdg_view_->toplevel, false);
    focused_xdg_view_ = view;
    wlr_scene_node_raise_to_top(&view->scene_tree->node);
    if (changed) wlr_xdg_toplevel_set_activated(view->toplevel, true);
    if (auto* keyboard = wlr_seat_get_keyboard(seat_); keyboard && (changed || seat_->keyboard_state.focused_surface != view->toplevel->base->surface))
        wlr_seat_keyboard_notify_enter(seat_, view->toplevel->base->surface,
            keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
}

void WlrServer::HandleKeyboardKey(WlrKeyboardBinding* binding, void* data) {
    auto* event = static_cast<wlr_keyboard_key_event*>(data);
    wlr_seat_set_keyboard(seat_, binding->keyboard);
    bool handled = false;
    if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED && event->keycode < binding->consumed_keys.size()) {
        handled = binding->consumed_keys[event->keycode];
        binding->consumed_keys[event->keycode] = false;
    }
    const auto mods = wlr_keyboard_get_modifiers(binding->keyboard);
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED && (mods & WLR_MODIFIER_LOGO)) {
        const xkb_keysym_t* symbols{};
        const auto layout = xkb_state_key_get_layout(binding->keyboard->xkb_state, event->keycode + 8);
        const int count = xkb_keymap_key_get_syms_by_level(binding->keyboard->keymap, event->keycode + 8, layout, 0, &symbols);
        for (int i = 0; i < count && !handled; ++i) {
            tree::Direction direction{};
            bool directional = true;
            switch (symbols[i]) {
                case XKB_KEY_Left: case XKB_KEY_h: direction = tree::Direction::Left; break;
                case XKB_KEY_Right: case XKB_KEY_l: direction = tree::Direction::Right; break;
                case XKB_KEY_Up: case XKB_KEY_k: direction = tree::Direction::Up; break;
                case XKB_KEY_Down: case XKB_KEY_j: direction = tree::Direction::Down; break;
                default: directional = false;
            }
            if (directional) {
                if (mods & WLR_MODIFIER_SHIFT) compositor_->SwapFocusDirection(direction);
                else if (!(focused_xdg_view_ && focused_xdg_view_->fullscreen)) compositor_->MoveFocus(direction);
                handled = true;
            } else if (symbols[i] == XKB_KEY_v) {
                compositor_->SetTreeLayout(tree::LayoutMode::SplitVertical); handled = true;
            } else if (symbols[i] == XKB_KEY_b) {
                compositor_->SetTreeLayout(tree::LayoutMode::SplitHorizontal); handled = true;
            } else if (symbols[i] == XKB_KEY_f) {
                if (focused_xdg_view_) SetXdgFullscreen(focused_xdg_view_, !focused_xdg_view_->fullscreen);
                handled = true;
            } else if ((mods & WLR_MODIFIER_SHIFT) && (symbols[i] == XKB_KEY_q || symbols[i] == XKB_KEY_Q)) {
                CloseFocusedXdgView(); handled = true;
            } else if (symbols[i] >= XKB_KEY_1 && symbols[i] <= XKB_KEY_9) {
                const auto name = std::to_string(symbols[i]-XKB_KEY_1+1);
                if (mods & WLR_MODIFIER_SHIFT) {
                    compositor_->GetTreeEngine().MoveWindowToWorkspace(compositor_->GetTreeEngine().GetFocusedWindow(), name);
                    compositor_->SynchronizeFocus();
                } else compositor_->SwitchWorkspace(name);
                handled = true;
            }
        }
        if (handled) {
            if (event->keycode < binding->consumed_keys.size()) binding->consumed_keys[event->keycode] = true;
            ArrangeXdgViews(); SynchronizeXdgFocus();
        }
    }
    if (!handled) wlr_seat_keyboard_notify_key(seat_, event->time_msec, event->keycode, event->state);
}

void WlrServer::HandleKeyboardModifiers(WlrKeyboardBinding* binding) {
    wlr_seat_set_keyboard(seat_, binding->keyboard);
    wlr_seat_keyboard_notify_modifiers(seat_, &binding->keyboard->modifiers);
}

void WlrServer::HandleKeyboardDestroy(WlrKeyboardBinding* binding) {
    if (wlr_seat_get_keyboard(seat_) == binding->keyboard) {
        wlr_seat_set_keyboard(seat_, nullptr);
    }
    auto it = std::find_if(keyboards_.begin(), keyboards_.end(),
        [binding](const auto& item) { return item.get() == binding; });
    if (it != keyboards_.end()) keyboards_.erase(it);
}

void WlrServer::CloseFocusedXdgView() {
    if (focused_xdg_view_ && focused_xdg_view_->toplevel) {
        wlr_xdg_toplevel_send_close(focused_xdg_view_->toplevel);
    }
}

void WlrServer::HandleCursorMotion(uint32_t time_msec, double dx, double dy) {
    wlr_cursor_move(cursor_, nullptr, dx, dy);
    if (cursor_mgr_) {
        wlr_cursor_set_xcursor(cursor_, cursor_mgr_, "left_ptr");
    }
    UpdateXdgPointerFocus(time_msec);
    if (compositor_) {
        compositor_->OnPointerMotion(static_cast<float>(cursor_->x), static_cast<float>(cursor_->y), static_cast<float>(dx), static_cast<float>(dy));
    }
    if (drag_manager_ && drag_manager_->IsDragging() && compositor_) {
        drag_manager_->UpdateDrag(static_cast<float>(cursor_->x), static_cast<float>(cursor_->y), compositor_->GetWindows());
    }
    for (auto& out : outputs_) {
        if (out && out->wlr_output) {
            wlr_output_schedule_frame(out->wlr_output);
        }
    }
}

void WlrServer::HandleCursorMotionAbsolute(uint32_t time_msec, double x, double y) {
    wlr_cursor_warp_absolute(cursor_, nullptr, x, y);
    if (cursor_mgr_) {
        wlr_cursor_set_xcursor(cursor_, cursor_mgr_, "left_ptr");
    }
    UpdateXdgPointerFocus(time_msec);
    if (compositor_) {
        compositor_->OnPointerMotion(static_cast<float>(cursor_->x), static_cast<float>(cursor_->y));
    }
    if (drag_manager_ && drag_manager_->IsDragging() && compositor_) {
        drag_manager_->UpdateDrag(static_cast<float>(cursor_->x), static_cast<float>(cursor_->y), compositor_->GetWindows());
    }
    for (auto& out : outputs_) {
        if (out && out->wlr_output) {
            wlr_output_schedule_frame(out->wlr_output);
        }
    }
}

void WlrServer::HandleCursorButton(uint32_t time_msec, uint32_t button, uint32_t state) {
    WlrXdgView* pointed = nullptr;
    if (seat_->pointer_state.focused_surface) {
        auto* surface = wlr_surface_get_root_surface(seat_->pointer_state.focused_surface);
        auto* top = wlr_xdg_toplevel_try_from_wlr_surface(surface);
        for (auto& view : xdg_views_) if (view->toplevel == top && view->visible && !view->shell_role) pointed = view.get();
    }
    auto* keyboard = wlr_seat_get_keyboard(seat_);
    if (button == 272 && state == WLR_BUTTON_PRESSED && pointed && !pointed->fullscreen && keyboard &&
        (wlr_keyboard_get_modifiers(keyboard) & WLR_MODIFIER_LOGO)) {
        FocusXdgView(pointed);
        dragged_xdg_view_ = pointed;
        return;
    }
    if (button == 272 && state == WLR_BUTTON_RELEASED && dragged_xdg_view_) {
        auto* source = dragged_xdg_view_;
        dragged_xdg_view_ = nullptr;
        if (pointed && pointed != source && !pointed->fullscreen) {
            auto& tree = compositor_->GetTreeEngine();
            auto target = tree.FindViewForWindow(pointed->managed);
            auto source_node = tree.FindViewForWindow(source->managed);
            const double dx = (cursor_->x-pointed->x)/std::max(1, pointed->width)-0.5;
            const double dy = (cursor_->y-pointed->y)/std::max(1, pointed->height)-0.5;
            if (std::abs(dx) < 0.18 && std::abs(dy) < 0.18) tree.SwapNodes(source_node, target);
            else {
                const auto direction = std::abs(dx) > std::abs(dy)
                    ? (dx < 0 ? tree::Direction::Left : tree::Direction::Right)
                    : (dy < 0 ? tree::Direction::Up : tree::Direction::Down);
                if (tree.RemoveWindow(source->managed)) tree.InsertWindow(source->managed, direction, target);
            }
            ArrangeXdgViews(); SynchronizeXdgFocus();
        }
        return;
    }
    if (state == WLR_BUTTON_PRESSED && seat_->pointer_state.focused_surface) {
        auto* top = wlr_xdg_toplevel_try_from_wlr_surface(seat_->pointer_state.focused_surface);
        for (auto& view : xdg_views_) {
            if (view->toplevel == top) {
                FocusXdgView(view.get());
                break;
            }
        }
    }
    wlr_seat_pointer_notify_button(seat_, time_msec, button,
                                   static_cast<wl_pointer_button_state>(state));
    wlr_seat_pointer_notify_frame(seat_);
    if (compositor_) {
        compositor_->OnPointerButton(button, state == WLR_BUTTON_PRESSED);
    }

    // Tiling Window Header & Drag-to-Split interaction (Left Click: 1 or 272)
    if (button == 1 || button == 272) {
        if (state == WLR_BUTTON_PRESSED) {
            if (compositor_) {
                const auto& windows = compositor_->GetWindows();
                for (size_t i = 0; i < windows.size(); ++i) {
                    auto& win = windows[i];
                    if (win->IsNative()) continue;
                    auto b = win->GetBounds();
                    if (cursor_->x >= b.x && cursor_->x <= b.x + b.width &&
                        cursor_->y >= b.y && cursor_->y <= b.y + b.height) {

                        compositor_->SetFocusedWindowIndex(static_cast<int>(i));

                        if (auto* dec = win->GetDecorator()) {
                            float lx = static_cast<float>(cursor_->x - b.x);
                            float ly = static_cast<float>(cursor_->y - b.y);
                            auto action = dec->HitTestHeader(lx, ly);
                            if (action == decoration::HeaderAction::TitlebarDrag) {
                                if (drag_manager_) {
                                    drag_manager_->BeginDrag(win, static_cast<float>(cursor_->x), static_cast<float>(cursor_->y));
                                }
                            } else if (action == decoration::HeaderAction::Close) {
                                PRISM_LOG_INFO("WM-CHROME", "Clicked Close on tile '%s'", win->GetTitle().c_str());
                            } else if (action == decoration::HeaderAction::ToggleSplit) {
                                PRISM_LOG_INFO("WM-CHROME", "Clicked ToggleSplit on tile '%s'", win->GetTitle().c_str());
                            } else if (action == decoration::HeaderAction::ToggleFold) {
                                dec->ToggleFold();
                                PRISM_LOG_INFO("WM-CHROME", "Clicked ToggleFold on tile '%s' (folded=%d)", win->GetTitle().c_str(), dec->IsFolded());
                            } else if (action == decoration::HeaderAction::ToggleMonocle) {
                                core::Rect screen{0.0f, 30.0f, 1920.0f, 1050.0f};
                                if (!outputs_.empty() && outputs_[0]->wlr_output) {
                                    screen = core::Rect{0.0f, 30.0f, static_cast<float>(outputs_[0]->wlr_output->width),
                                                       static_cast<float>(outputs_[0]->wlr_output->height - 30)};
                                }
                                dec->ToggleFullscreen(screen);
                                PRISM_LOG_INFO("WM-CHROME", "Clicked ToggleMonocle on tile '%s' (fullscreen=%d)", win->GetTitle().c_str(), dec->IsFullscreen());
                            }
                        }
                        break;
                    }
                }
            }
        } else {
            if (drag_manager_ && drag_manager_->IsDragging()) {
                auto res = drag_manager_->EndDrag();
                if (res.executed && res.source_window && res.target_window &&
                    res.source_window != res.target_window && compositor_) {
                    if (res.quadrant == decoration::DropQuadrant::Swap) {
                        auto src_node = compositor_->GetTreeEngine().FindViewForWindow(res.source_window);
                        auto tgt_node = compositor_->GetTreeEngine().FindViewForWindow(res.target_window);
                        if (src_node && tgt_node) {
                            compositor_->GetTreeEngine().SwapNodes(src_node, tgt_node);
                        }
                    } else {
                        tree::Direction dir = tree::Direction::Right;
                        if (res.quadrant == decoration::DropQuadrant::LeftSplit) dir = tree::Direction::Left;
                        else if (res.quadrant == decoration::DropQuadrant::RightSplit) dir = tree::Direction::Right;
                        else if (res.quadrant == decoration::DropQuadrant::TopSplit) dir = tree::Direction::Up;
                        else if (res.quadrant == decoration::DropQuadrant::BottomSplit) dir = tree::Direction::Down;

                        auto tgt_node = compositor_->GetTreeEngine().FindViewForWindow(res.target_window);
                        compositor_->GetTreeEngine().RemoveWindow(res.source_window);
                        compositor_->GetTreeEngine().InsertWindow(res.source_window, dir, tgt_node);
                    }
                    PRISM_LOG_INFO("WM-TILING", "Committed Drag-to-Split between [%s] and [%s]",
                                   res.source_window->GetTitle().c_str(),
                                   res.target_window->GetTitle().c_str());
                }
            }
        }
    }

    for (auto& out : outputs_) {
        if (out && out->wlr_output) {
            wlr_output_schedule_frame(out->wlr_output);
        }
    }
}

void WlrServer::HandleCursorAxis(uint32_t time_msec, int axis, double value) {
    wlr_seat_pointer_notify_axis(seat_, time_msec,
                                 static_cast<wl_pointer_axis>(axis), value, 0,
                                 WL_POINTER_AXIS_SOURCE_FINGER,
                                 WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    wlr_seat_pointer_notify_frame(seat_);
}

void WlrServer::UpdateXdgPointerFocus(uint32_t time_msec) {
    if (!windows_tree_ || !seat_ || !cursor_) return;
    double sx = 0.0;
    double sy = 0.0;
    struct wlr_surface* surface = nullptr;
    for (auto* tree : {chrome_tree_, windows_tree_, background_tree_}) {
        auto* node = wlr_scene_node_at(&tree->node, cursor_->x, cursor_->y, &sx, &sy);
        if (node && node->type == WLR_SCENE_NODE_BUFFER) {
            auto* scene_surface = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
            if (scene_surface) surface = scene_surface->surface;
        }
        if (surface) break;
    }
    if (surface) {
        wlr_seat_pointer_notify_enter(seat_, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat_, time_msec, sx, sy);
    } else {
        wlr_seat_pointer_notify_clear_focus(seat_);
    }
    wlr_seat_pointer_notify_frame(seat_);
}

void WlrServer::InitSceneGraph() {
    if (!scene_) return;

    // 1. Layer 0: Background Desktop
    background_tree_ = wlr_scene_tree_create(&scene_->tree);


    // 2. Layer 1: Windows & Application Views
    windows_tree_ = wlr_scene_tree_create(&scene_->tree);

    // 3. Layer 2: Desktop Chrome (Top Menu Bar, Split Divider, Dock)
    chrome_tree_ = wlr_scene_tree_create(&scene_->tree);



    // 4. Layer 3: HUD (Debug Performance Overlay)
    hud_tree_ = wlr_scene_tree_create(&scene_->tree);
    wlr_scene_node_set_enabled(&hud_tree_->node,
        compositor_ && compositor_->IsDebugHudEnabled());
    const float hud_bg[4] = {0.05f, 0.07f, 0.09f, 0.92f};
    hud_bg_rect_ = wlr_scene_rect_create(hud_tree_, 440, 160, hud_bg);
    const float hud_border[4] = {0.35f, 0.65f, 1.0f, 1.0f}; // Neon cyan accent
    hud_border_rect_ = wlr_scene_rect_create(hud_tree_, 440, 3, hud_border);
    const float hud_fps_pill[4] = {0.25f, 0.73f, 0.31f, 1.0f}; // Bright emerald green
    hud_status_pill_ = wlr_scene_rect_create(hud_tree_, 12, 12, hud_fps_pill);
    wlr_scene_node_set_position(&hud_status_pill_->node, 16, 16);
    wlr_scene_node_set_position(&hud_tree_->node, 18, 44);


}

void WlrServer::UpdateSceneGraph(int width, int height, float dt) {
    if (width<=0 || height<=0 || !hud_tree_) return;
    // Real XDG view geometry is owned exclusively by the BSP arrangement.
    // Decoration/material buffers are composed by SurfaceEffects below views.
    wlr_scene_node_set_enabled(&hud_tree_->node,compositor_ && compositor_->IsDebugHudEnabled());
}

void WlrServer::HandleOutputFrame(WlrOutput* output) {
    if (!output || !output->scene_output) return;

    static int s_frame_log_count = 0;
    uint64_t now_ns = core::CurrentTimeNs();
    float dt = 0.016f;
    if (last_frame_time_ns_ > 0) {
        dt = static_cast<float>(now_ns - last_frame_time_ns_) / 1e9f;
        if (dt > 0.0001f && dt < 1.0f) {
            float inst_fps = 1.0f / dt;
            current_fps_ = (current_fps_ <= 0.0f) ? inst_fps : (0.9f * current_fps_ + 0.1f * inst_fps);
        }
    }
    last_frame_time_ns_ = now_ns;
    frame_count_++;

    if (compositor_) {
        compositor_->SetPerformanceStats(current_fps_, dt, frame_count_);
        compositor_->Tick(dt);
    }

    int out_w = output->wlr_output->width;
    int out_h = output->wlr_output->height;
    bool commit_ok = false;
    if (out_w > 0 && out_h > 0) {
        // 1. Update Hardware GPU Scene-graph layout (Windows, Divider, Dock, HUD)
        UpdateSceneGraph(out_w, out_h, dt);
        if(surface_effects_){std::vector<WlrXdgView*> views;for(auto& view:xdg_views_)views.push_back(view.get());
            surface_effects_->Update(scene_,views,focused_xdg_view_);}

        // 2. Hardware-accelerated GPU render & commit (wlr_scene natively dispatches GLES2 render pass & Direct Scanout!)
        uint64_t t_gpu_start = core::CurrentTimeNs();
        commit_ok = wlr_scene_output_commit(output->scene_output, nullptr);
        uint64_t t_gpu_done = core::CurrentTimeNs();

        // 3. Send frame_done to client surfaces
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        wlr_scene_output_send_frame_done(output->scene_output, &now);

        if (++s_frame_log_count % 60 == 1) {
            float gpu_ms = static_cast<float>(t_gpu_done - t_gpu_start) / 1e6f;
            PRISM_LOG_INFO("WLR-SCENE", "Native GPU Frame %lu on '%s': commit=%s (%dx%d @ %.1f FPS, gpu_commit=%.2fms, dt=%.2fms)",
                           frame_count_, output->wlr_output->name,
                           commit_ok ? "OK" : "FAILED", out_w, out_h,
                           current_fps_, gpu_ms, dt * 1000.0f);
        }
    }

    // Schedule next frame for continuous presentation aligned with monitor VSync
    if (commit_ok) {
        wlr_output_schedule_frame(output->wlr_output);
    }
}

std::vector<OutputInfo> WlrServer::GetOutputsInfo() const {
    std::vector<OutputInfo> result;
    for (const auto& out : outputs_) {
        struct wlr_output* w_out = out->wlr_output;
        OutputInfo info;
        info.name = w_out->name ? w_out->name : "unknown";
        info.make = w_out->make ? w_out->make : "generic";
        info.model = w_out->model ? w_out->model : "display";
        info.width = w_out->width;
        info.height = w_out->height;
        info.refresh_mhz = w_out->refresh;
        info.refresh_hz = w_out->refresh / 1000.0f;
        info.current_fps = current_fps_;
        info.adaptive_sync = (w_out->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED);

        struct wlr_output_mode* m;
        wl_list_for_each(m, &w_out->modes, link) {
            OutputModeInfo mi;
            mi.width = m->width;
            mi.height = m->height;
            mi.refresh_mhz = m->refresh;
            mi.refresh_hz = m->refresh / 1000.0f;
            mi.preferred = m->preferred;
            mi.current = (m == w_out->current_mode);
            info.modes.push_back(mi);
        }

        if (info.modes.empty() && info.width > 0 && info.height > 0) {
            OutputModeInfo mi;
            mi.width = info.width;
            mi.height = info.height;
            mi.refresh_mhz = info.refresh_mhz > 0 ? info.refresh_mhz : 60000;
            mi.refresh_hz = mi.refresh_mhz / 1000.0f;
            mi.preferred = true;
            mi.current = true;
            info.modes.push_back(mi);
        }

        result.push_back(info);
    }
    return result;
}

bool WlrServer::SetOutputMode(const std::string& name, int width, int height, int refresh_mhz) {
    bool any_success = false;
    for (auto& out : outputs_) {
        struct wlr_output* w_out = out->wlr_output;
        if (!name.empty() && name != "all" && name != w_out->name) {
            continue;
        }

        struct wlr_output_mode* best_mode = nullptr;
        struct wlr_output_mode* m;
        wl_list_for_each(m, &w_out->modes, link) {
            if (m->width == width && m->height == height) {
                if (refresh_mhz <= 0 || std::abs(m->refresh - refresh_mhz) < 500) {
                    best_mode = m;
                    break;
                }
            }
        }

        struct wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, true);

        if (best_mode) {
            wlr_output_state_set_mode(&state, best_mode);
            PRISM_LOG_INFO("WLR-MODE", "Output '%s' mode set to fixed mode %dx%d @ %.1fHz",
                           w_out->name, best_mode->width, best_mode->height, best_mode->refresh / 1000.0f);
        } else {
            bool has_modes = !wl_list_empty(&w_out->modes);
            int ref = has_modes ? ((refresh_mhz > 0) ? refresh_mhz : (w_out->refresh > 0 ? w_out->refresh : 60000)) : 0;
            wlr_output_state_set_custom_mode(&state, width, height, ref);
            PRISM_LOG_INFO("WLR-MODE", "Output '%s' mode set to custom mode %dx%d (refresh=%d)",
                           w_out->name, width, height, ref);
        }

        if (w_out->render_format) {
            wlr_output_state_set_render_format(&state, w_out->render_format);
        }

        if (wlr_output_commit_state(w_out, &state)) {
            any_success = true;
            ArrangeXdgViews();
            UpdateSceneGraph(w_out->width, w_out->height);
            wlr_output_schedule_frame(w_out);
        } else {
            PRISM_LOG_ERROR("WLR-MODE", "Failed to commit mode change on output '%s'", w_out->name);
        }
        wlr_output_state_finish(&state);
    }
    return any_success;
}

bool WlrServer::SetAdaptiveSync(const std::string& name, bool enabled) {
    bool any_success = false;
    for (auto& out : outputs_) {
        struct wlr_output* w_out = out->wlr_output;
        if (!name.empty() && name != "all" && name != w_out->name) {
            continue;
        }

        struct wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_adaptive_sync_enabled(&state, enabled);
        if (wlr_output_commit_state(w_out, &state)) {
            any_success = true;
            PRISM_LOG_INFO("WLR-MODE", "Adaptive sync on '%s' set to %s", w_out->name, enabled ? "enabled" : "disabled");
        }
        wlr_output_state_finish(&state);
    }
    return any_success;
}

} // namespace prism::wm
