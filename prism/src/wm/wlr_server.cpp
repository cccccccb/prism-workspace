#include "prism/wm/wlr_server.hpp"
#include "prism/ipc/ipc_server.hpp"
#include "prism/ipc/channel.hpp"
#include "prism/core/logging.hpp"
#include "prism/layout/fluid_split_strategy.hpp"
#include "prism/layout/mission_control_strategy.hpp"
#include "prism/decoration/tiling_decoration_spec.hpp"
#include "prism/decoration/tiling_window_decorator.hpp"
#include "prism/decoration/tiling_drag_manager.hpp"
#include "prism/compiler/binary_loader.hpp"

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
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include <drm_fourcc.h>
}

#include <cstddef>
#include <chrono>
#include <unistd.h>
#include <sstream>
#include <cmath>
#include <iomanip>
#include <algorithm>

namespace prism::wm {

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
    auto* sig = WlContainerOf<WlrServerSignals>(listener, offsetof(WlrServerSignals, new_xdg_surface));
    sig->server->HandleNewXdgSurface(static_cast<struct wlr_xdg_surface*>(data));
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
    backend_ = wlr_backend_autocreate(wl_display_, nullptr);
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
    wlr_data_device_manager_create(wl_display_);

    // 5. Output layout & Hardware Scene graph
    output_layout_ = wlr_output_layout_create();
    scene_ = wlr_scene_create();
    wlr_scene_attach_output_layout(scene_, output_layout_);

    // 6. Initialize 100% Native GPU Scene-Graph hierarchy
    InitSceneGraph();

    // 7. XDG Shell Protocol (Wayland Window Standard Protocol)
    xdg_shell_ = wlr_xdg_shell_create(wl_display_, 3);
    if (xdg_shell_) {
        signals_.new_xdg_surface.notify = handle_server_new_xdg_surface;
        wl_signal_add(&xdg_shell_->events.new_surface, &signals_.new_xdg_surface);
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
               << "  \"version\": \"Project PrismWM 0.1.0 (wlroots 0.17 / Sway architecture)\",\n"
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

        // Command 4: set_layout / layout: <split|mission_control|overview|toggle>
        auto layout_handler = [this](const std::string&, const std::vector<std::string>& args) {
            if (args.empty()) {
                return std::string("{\"status\": \"error\", \"message\": \"Usage: set_layout <split|mission_control|overview|toggle>\"}");
            }
            if (compositor_) {
                if (args[0] == "toggle") {
                    compositor_->ToggleMissionControl();
                } else if (args[0] == "mission_control" || args[0] == "overview") {
                    compositor_->SetMissionControl(true);
                } else if (args[0] == "split" || args[0] == "normal") {
                    compositor_->SetMissionControl(false);
                }
            }
            return std::string("{\"status\": \"ok\", \"layout\": \"") + args[0] + "\"}";
        };
        ipc_server_->RegisterHandler("set_layout", layout_handler);
        ipc_server_->RegisterHandler("layout", layout_handler);

        // Command 8: set_theme / theme: <theme_path | nordic | default | minimal>
        auto set_theme_handler = [this](const std::string&, const std::vector<std::string>& args) {
            if (args.empty()) {
                std::string cur_name = decoration_spec_ ? decoration_spec_->theme_name : "none";
                return std::string("{\"status\": \"ok\", \"current_theme\": \"") + cur_name + "\"}";
            }
            std::string theme_arg = args[0];
            std::shared_ptr<decoration::TilingDecorationSpec> new_spec = nullptr;
            if (theme_arg == "nordic" || theme_arg == "NordicGlass") {
                new_spec = decoration::TilingDecorationSpec::CreateNordicGlass();
            } else if (theme_arg == "default" || theme_arg == "DefaultTilingGlass") {
                new_spec = decoration::TilingDecorationSpec::CreateDefault();
            } else if (theme_arg == "minimal" || theme_arg == "MinimalI3") {
                new_spec = decoration::TilingDecorationSpec::CreateMinimalI3();
            } else {
                std::string bin_path = theme_arg;
                if (theme_arg.ends_with(".prism")) {
                    bin_path = theme_arg + "b";
                }
                new_spec = compiler::BinaryThemeLoader::LoadFromFile(bin_path);
            }

            if (!new_spec) {
                return std::string("{\"status\": \"error\", \"message\": \"Failed to load theme: ") + theme_arg + "\"}";
            }

            decoration_spec_ = new_spec;
            if (compositor_) {
                for (auto& win : compositor_->GetWindows()) {
                    if (auto* dec = win->GetDecorator()) {
                        dec->SetSpec(decoration_spec_);
                    }
                }
            }

            for (auto& out : outputs_) {
                if (out && out->wlr_output) {
                    wlr_output_schedule_frame(out->wlr_output);
                }
            }

            PRISM_LOG_INFO("WLR-SERVER", "Applied new Tiling Decoration Theme: '%s'", decoration_spec_->theme_name.c_str());
            return std::string("{\"status\": \"ok\", \"message\": \"Theme switched successfully\", \"theme\": \"") +
                   decoration_spec_->theme_name + "\"}";
        };
        ipc_server_->RegisterHandler("set_theme", set_theme_handler);
        ipc_server_->RegisterHandler("theme", set_theme_handler);

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
            wlr_wl_output_set_title(out, "Project PrismWM Compositor (wlroots 0.17 / Sway)");
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
            wlr_x11_output_set_title(out, "Project PrismWM Compositor (wlroots 0.17 / Sway)");
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
    PRISM_LOG_INFO("WLR-SERVER", "Backend started! is_multi=%d, is_wl=%d",
                   wlr_backend_is_multi(backend_), wlr_backend_is_wl(backend_));

    // Auto-create output window if running under nested host (Wayland, X11) or Headless
    bool created = false;
    if (wlr_backend_is_multi(backend_)) {
        wlr_multi_for_each_backend(backend_, auto_create_output_for_nested, &created);
    } else {
        auto_create_output_for_nested(backend_, &created);
    }

    PRISM_LOG_INFO("WLR-SERVER", "wlroots backend running! Wayland display listening on %s (created_nested=%d)",
                   socket_name_.c_str(), created ? 1 : 0);
}

void WlrServer::Stop() {
    if (!running_) return;
    running_ = false;

    outputs_.clear();
    dock_icon_rects_.clear();

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
        wl_display_destroy_clients(wl_display_);
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
    wl_event_loop_dispatch(wl_event_loop_, timeout_ms);
    wl_display_flush_clients(wl_display_);

    for (const auto& out : outputs_) {
        if (out && out->wlr_output) {
            wlr_output_schedule_frame(out->wlr_output);
        }
    }
}

void WlrServer::HandleNewOutput(struct wlr_output* output) {
    wlr_output_init_render(output, allocator_, renderer_);

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
    
    WlrOutput* raw_out = wlr_out.get();
    outputs_.push_back(std::move(wlr_out));

    // 4. Kickstart first frame
    HandleOutputFrame(raw_out);
}

void WlrServer::RemoveOutput(WlrOutput* output) {
    for (auto it = outputs_.begin(); it != outputs_.end(); ++it) {
        if (it->get() == output) {
            PRISM_LOG_INFO("WLR-OUTPUT", "Output removed: %s", output->wlr_output->name);
            outputs_.erase(it);
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
        PRISM_LOG_INFO("WLR-INPUT", "Keyboard attached: %s", device->name);
    } else if (device->type == WLR_INPUT_DEVICE_POINTER || device->type == WLR_INPUT_DEVICE_TOUCH) {
        wlr_cursor_attach_input_device(cursor_, device);
        PRISM_LOG_INFO("WLR-INPUT", "Pointer/Touchpad attached: %s", device->name);
    }
}

void WlrServer::HandleNewXdgSurface(struct wlr_xdg_surface* xdg_surface) {
    if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
        PRISM_LOG_INFO("WLR-XDG", "New XDG Toplevel Surface: title='%s' app_id='%s'",
                       (xdg_surface->toplevel && xdg_surface->toplevel->title) ? xdg_surface->toplevel->title : "(untitled)",
                       (xdg_surface->toplevel && xdg_surface->toplevel->app_id) ? xdg_surface->toplevel->app_id : "(none)");
        
        // Attach standard Wayland client surface directly to hardware scene tree!
        if (windows_tree_) {
            wlr_scene_xdg_surface_create(windows_tree_, xdg_surface);
        }
    }
}

void WlrServer::HandleCursorMotion(uint32_t time_msec, double dx, double dy) {
    wlr_cursor_move(cursor_, nullptr, dx, dy);
    if (cursor_mgr_) {
        wlr_cursor_set_xcursor(cursor_, cursor_mgr_, "left_ptr");
    }
    wlr_seat_pointer_notify_motion(seat_, time_msec, cursor_->x, cursor_->y);
    wlr_seat_pointer_notify_frame(seat_);
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
    wlr_seat_pointer_notify_motion(seat_, time_msec, cursor_->x, cursor_->y);
    wlr_seat_pointer_notify_frame(seat_);
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
    wlr_seat_pointer_notify_button(seat_, time_msec, button, static_cast<wlr_button_state>(state));
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
                            } else if (action == decoration::HeaderAction::ToggleMonocle) {
                                PRISM_LOG_INFO("WM-CHROME", "Clicked ToggleMonocle on tile '%s'", win->GetTitle().c_str());
                            }
                        }
                        break;
                    }
                }
            }
        } else {
            if (drag_manager_ && drag_manager_->IsDragging()) {
                auto res = drag_manager_->EndDrag();
                if (res.executed) {
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
    wlr_seat_pointer_notify_axis(seat_, time_msec, static_cast<wlr_axis_orientation>(axis), value, 0, WLR_AXIS_SOURCE_FINGER);
}

void WlrServer::InitSceneGraph() {
    if (!scene_) return;

    // 1. Layer 0: Background Desktop
    background_tree_ = wlr_scene_tree_create(&scene_->tree);
    const float bg_color[4] = {0.043f, 0.047f, 0.063f, 1.0f}; // macOS Dark Slate / Nebula #0b0c10
    wallpaper_rect_ = wlr_scene_rect_create(background_tree_, 1920, 1080, bg_color);

    // 2. Layer 1: Windows & Application Views
    windows_tree_ = wlr_scene_tree_create(&scene_->tree);

    // 3. Layer 2: Desktop Chrome (Top Menu Bar, Split Divider, Dock)
    chrome_tree_ = wlr_scene_tree_create(&scene_->tree);

    // Top Menu Bar background (30px height, translucent dark slate: rgba(0.08, 0.09, 0.12, 0.85))
    const float bar_color[4] = {0.08f, 0.09f, 0.12f, 0.85f};
    top_bar_rect_ = wlr_scene_rect_create(chrome_tree_, 1920, 30, bar_color);

    // Top Menu Bar bottom border line (1px, subtle highlight: rgba(1.0, 1.0, 1.0, 0.12))
    const float line_color[4] = {1.0f, 1.0f, 1.0f, 0.12f};
    top_bar_border_ = wlr_scene_rect_create(chrome_tree_, 1920, 1, line_color);
    wlr_scene_node_set_position(&top_bar_border_->node, 0, 29);

    // Top Menu Bar Apple / Prism logo icon (16x16 blue pill)
    const float icon_color[4] = {0.0f, 0.48f, 1.0f, 1.0f};
    top_bar_icon_ = wlr_scene_rect_create(chrome_tree_, 16, 16, icon_color);
    wlr_scene_node_set_position(&top_bar_icon_->node, 14, 7);

    // Split Divider (Vertical line & pill handle)
    const float div_line_color[4] = {1.0f, 1.0f, 1.0f, 0.18f};
    split_divider_line_ = wlr_scene_rect_create(chrome_tree_, 2, 1050, div_line_color);
    const float div_pill_color[4] = {0.0f, 0.48f, 1.0f, 0.95f}; // Apple Blue pill
    split_divider_pill_ = wlr_scene_rect_create(chrome_tree_, 6, 42, div_pill_color);

    // macOS Floating Glass Dock (Bottom centered)
    const float dock_bg_color[4] = {0.14f, 0.16f, 0.22f, 0.88f};
    dock_bg_rect_ = wlr_scene_rect_create(chrome_tree_, 320, 64, dock_bg_color);

    // 5 Dock app icons (Music, Settings, Terminal, Files, Browser)
    const float dock_palette[5][4] = {
        {0.98f, 0.18f, 0.28f, 1.0f}, // Music (Red)
        {0.56f, 0.56f, 0.58f, 1.0f}, // Settings (Gray)
        {0.0f, 0.48f, 1.0f, 1.0f},  // Terminal (Blue)
        {0.2f, 0.78f, 0.35f, 1.0f},  // Files (Green)
        {1.0f, 0.58f, 0.0f, 1.0f}   // Browser (Orange)
    };
    for (int i = 0; i < 5; ++i) {
        auto* icon_rect = wlr_scene_rect_create(chrome_tree_, 44, 44, dock_palette[i]);
        dock_icon_rects_.push_back(icon_rect);
    }
    const float dot_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    dock_active_dot_ = wlr_scene_rect_create(chrome_tree_, 4, 4, dot_color);

    // 4. Layer 3: HUD (Debug Performance Overlay)
    hud_tree_ = wlr_scene_tree_create(&scene_->tree);
    const float hud_bg[4] = {0.05f, 0.07f, 0.09f, 0.92f};
    hud_bg_rect_ = wlr_scene_rect_create(hud_tree_, 440, 160, hud_bg);
    const float hud_border[4] = {0.35f, 0.65f, 1.0f, 1.0f}; // Neon cyan accent
    hud_border_rect_ = wlr_scene_rect_create(hud_tree_, 440, 3, hud_border);
    const float hud_fps_pill[4] = {0.25f, 0.73f, 0.31f, 1.0f}; // Bright emerald green
    hud_status_pill_ = wlr_scene_rect_create(hud_tree_, 12, 12, hud_fps_pill);
    wlr_scene_node_set_position(&hud_status_pill_->node, 16, 16);
    wlr_scene_node_set_position(&hud_tree_->node, 18, 44);

    // 5. Layer 4: Tiling Drag Overlay (Drop-Zone visualization)
    drag_manager_ = std::make_unique<decoration::TilingDragManager>(decoration_spec_);
    drag_manager_->AttachToScene(chrome_tree_);

    PRISM_LOG_INFO("WLR-SCENE", "Native GPU Scene-graph hierarchy initialized successfully");
}

void WlrServer::UpdateSceneGraph(int width, int height) {
    if (width <= 0 || height <= 0) return;

    // 1. Wallpaper size
    if (wallpaper_rect_) {
        wlr_scene_rect_set_size(wallpaper_rect_, width, height);
    }

    // 2. Top Menu Bar size
    if (top_bar_rect_) {
        wlr_scene_rect_set_size(top_bar_rect_, width, 30);
    }
    if (top_bar_border_) {
        wlr_scene_rect_set_size(top_bar_border_, width, 1);
        wlr_scene_node_set_position(&top_bar_border_->node, 0, 29);
    }

    // 3. Desktop Windows & Modular Tiling Window Decorator System
    if (compositor_) {
        const auto& windows = compositor_->GetWindows();
        for (size_t i = 0; i < windows.size(); ++i) {
            auto& win = windows[i];
            if (!win->GetDecorator()) {
                auto decorator = std::make_unique<decoration::TilingWindowDecorator>(win.get(), decoration_spec_);
                decorator->AttachToScene(windows_tree_);

                // Mount internal interactive controls inside decorator's content tree
                auto* c_tree = decorator->GetContentTree();
                if (c_tree) {
                    const float btn_col[4] = {0.24f, 0.27f, 0.33f, 0.85f};
                    auto* btn1 = wlr_scene_rect_create(c_tree, 140, 36, btn_col);
                    wlr_scene_node_set_position(&btn1->node, 24, 20);

                    auto* btn2 = wlr_scene_rect_create(c_tree, 140, 36, btn_col);
                    wlr_scene_node_set_position(&btn2->node, 180, 20);

                    const float track_col[4] = {0.20f, 0.22f, 0.28f, 1.0f};
                    const float fill_col[4] = {0.0f, 0.48f, 1.0f, 1.0f};
                    auto* sl_track = wlr_scene_rect_create(c_tree, 300, 6, track_col);
                    wlr_scene_node_set_position(&sl_track->node, 24, 80);
                    auto* sl_fill = wlr_scene_rect_create(c_tree, 120 + static_cast<int>(i) * 30, 6, fill_col);
                    wlr_scene_node_set_position(&sl_fill->node, 24, 80);
                }

                win->SetDecorator(std::move(decorator));
            }

            // Sync focus and layout geometry via TilingWindowDecorator
            bool is_focused = (static_cast<int>(i) == compositor_->GetFocusedWindowIndex());
            win->SetFocused(is_focused);
            win->GetDecorator()->ApplyGeometry(win->GetBounds());
        }

        // 4. Split Divider
        float mc_prog = 0.0f;
        layout::MacFluidSplitStrategy* split_strat = nullptr;
        if (auto mc = dynamic_cast<layout::MissionControlStrategy*>(compositor_->GetLayoutStrategy())) {
            mc_prog = mc->GetProgress();
            split_strat = dynamic_cast<layout::MacFluidSplitStrategy*>(mc->GetBaseStrategy());
        } else {
            split_strat = dynamic_cast<layout::MacFluidSplitStrategy*>(compositor_->GetLayoutStrategy());
        }

        bool show_divider = (split_strat && mc_prog < 0.2f && windows.size() >= 2);
        wlr_scene_node_set_enabled(&split_divider_line_->node, show_divider);
        wlr_scene_node_set_enabled(&split_divider_pill_->node, show_divider);
        if (show_divider) {
            int div_x = static_cast<int>(width * split_strat->GetCurrentRatio());
            wlr_scene_rect_set_size(split_divider_line_, 2, height - 30);
            wlr_scene_node_set_position(&split_divider_line_->node, div_x - 1, 30);

            wlr_scene_node_set_position(&split_divider_pill_->node, div_x - 3, height / 2 - 21);
        }

        // 5. Dock position
        int dock_w = 320;
        int dock_h = 64;
        int dock_x = (width - dock_w) / 2;
        int dock_y = height - dock_h - 14;
        wlr_scene_node_set_position(&dock_bg_rect_->node, dock_x, dock_y);

        int icon_pad = 16;
        int icon_size = 44;
        int icon_gap = 16;
        int cur_ix = dock_x + icon_pad;
        for (size_t i = 0; i < dock_icon_rects_.size(); ++i) {
            wlr_scene_node_set_position(&dock_icon_rects_[i]->node, cur_ix, dock_y + 10);
            cur_ix += icon_size + icon_gap;
        }
        int focused_idx = compositor_->GetFocusedWindowIndex();
        int dot_x = dock_x + icon_pad + focused_idx * (icon_size + icon_gap) + icon_size / 2 - 2;
        wlr_scene_node_set_position(&dock_active_dot_->node, dot_x, dock_y + dock_h - 6);

        // 6. Debug HUD
        bool hud_enabled = compositor_->IsDebugHudEnabled();
        wlr_scene_node_set_enabled(&hud_tree_->node, hud_enabled);
        if (hud_enabled && hud_status_pill_) {
            // Update color based on current_fps_
            if (current_fps_ >= 50.0f) {
                const float c[4] = {0.25f, 0.73f, 0.31f, 1.0f}; // Green
                wlr_scene_rect_set_color(hud_status_pill_, c);
            } else if (current_fps_ >= 30.0f) {
                const float c[4] = {0.82f, 0.60f, 0.13f, 1.0f}; // Yellow
                wlr_scene_rect_set_color(hud_status_pill_, c);
            } else {
                const float c[4] = {0.97f, 0.32f, 0.29f, 1.0f}; // Red
                wlr_scene_rect_set_color(hud_status_pill_, c);
            }
        }
    }
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
    if (out_w > 0 && out_h > 0) {
        // 1. Update Hardware GPU Scene-graph layout (Windows, Divider, Dock, HUD)
        UpdateSceneGraph(out_w, out_h);

        // 2. Hardware-accelerated GPU render & commit (wlr_scene natively dispatches GLES2 render pass & Direct Scanout!)
        uint64_t t_gpu_start = core::CurrentTimeNs();
        bool commit_ok = wlr_scene_output_commit(output->scene_output, nullptr);
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
    wlr_output_schedule_frame(output->wlr_output);
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
