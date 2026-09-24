#pragma once

#include "prism/core/noncopyable.hpp"
#include "prism/core/types.hpp"
#include "prism/wm/compositor.hpp"
#include "prism/decoration/tiling_decoration_spec.hpp"
#include "prism/decoration/tiling_drag_manager.hpp"

#include <string>
#include <memory>
#include <vector>

#include <wayland-server-core.h>

// Forward declarations of wlroots and wayland C structures
struct wl_display;
struct wl_event_loop;
struct wlr_backend;
struct wlr_renderer;
struct wlr_allocator;
struct wlr_compositor;
struct wlr_subcompositor;
struct wlr_output_layout;
struct wlr_scene;
struct wlr_scene_output;
struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_scene_buffer;
struct wlr_scene_node;
struct wlr_xdg_shell;
struct wlr_xdg_surface;
struct wlr_seat;
struct wlr_cursor;
struct wlr_xcursor_manager;
struct wlr_output;
struct wlr_input_device;
struct wlr_keyboard;

namespace prism::ipc {
class IpcServer;
}

namespace prism::wm {

struct OutputModeInfo {
    int width{0};
    int height{0};
    int refresh_mhz{0};
    float refresh_hz{0.0f};
    bool preferred{false};
    bool current{false};
};

struct OutputInfo {
    std::string name;
    std::string make;
    std::string model;
    int width{0};
    int height{0};
    int refresh_mhz{0};
    float refresh_hz{0.0f};
    float current_fps{0.0f};
    bool adaptive_sync{false};
    std::vector<OutputModeInfo> modes;
};

class WlrServer;

struct WlrOutput {
    WlrOutput(struct wlr_output* out, WlrServer* server);
    ~WlrOutput();

    struct wlr_output* wlr_output{nullptr};
    struct wlr_scene_output* scene_output{nullptr};
    WlrServer* server{nullptr};
    struct wl_listener frame;
    struct wl_listener request_state;
    struct wl_listener destroy;
};

struct WlrServerSignals {
    struct wl_listener new_output;
    struct wl_listener new_input;
    struct wl_listener new_xdg_surface;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;
    WlrServer* server{nullptr};
};

/**
 * @brief Wayland & wlroots Compositor Server Engine (Sway architecture)
 *        100% Native GPU Hardware Compositing via wlr_scene.
 *        Manages display lifecycle, multi-output DRM/KMS scanout, input seats,
 *        hardware cursors, and sub-millisecond GPU scene presentation.
 */
class WlrServer : public core::NonCopyable {
public:
    explicit WlrServer(std::shared_ptr<Compositor> compositor);
    ~WlrServer();

    bool Initialize(const std::string& socket_name = "");
    void Start();
    void Stop();

    void RunEventLoopIteration(int timeout_ms = 0);

    struct wl_display* GetDisplay() const { return wl_display_; }
    struct wl_event_loop* GetEventLoop() const { return wl_event_loop_; }
    struct wlr_backend* GetBackend() const { return backend_; }
    struct wlr_renderer* GetRenderer() const { return renderer_; }
    struct wlr_output_layout* GetOutputLayout() const { return output_layout_; }
    struct wlr_scene* GetScene() const { return scene_; }
    struct wlr_cursor* GetCursor() const { return cursor_; }
    struct wlr_seat* GetSeat() const { return seat_; }

    const std::string& GetSocketName() const { return socket_name_; }
    bool IsRunning() const { return running_; }

    void HandleNewOutput(struct wlr_output* output);
    void HandleNewInput(struct wlr_input_device* device);
    void HandleNewXdgSurface(struct wlr_xdg_surface* xdg_surface);
    void HandleCursorMotion(uint32_t time_msec, double dx, double dy);
    void HandleCursorMotionAbsolute(uint32_t time_msec, double x, double y);
    void HandleCursorButton(uint32_t time_msec, uint32_t button, uint32_t state);
    void HandleCursorAxis(uint32_t time_msec, int axis, double value);
    void HandleOutputFrame(WlrOutput* output);
    void RemoveOutput(WlrOutput* output);

    std::shared_ptr<Compositor> GetCompositor() const { return compositor_; }

    // Dynamic Display & Mode control (Sway architecture)
    bool SetOutputMode(const std::string& name, int width, int height, int refresh_mhz = 0);
    bool SetAdaptiveSync(const std::string& name, bool enabled);
    std::vector<OutputInfo> GetOutputsInfo() const;

    float GetCurrentFps() const { return current_fps_; }
    uint64_t GetFrameCount() const { return frame_count_; }
    ipc::IpcServer* GetIpcServer() const { return ipc_server_.get(); }

private:
    void InitSceneGraph();
    void UpdateSceneGraph(int width, int height, float dt = 0.016f);

    WlrServerSignals signals_{};
    std::shared_ptr<Compositor> compositor_;
    std::string socket_name_;
    bool running_{false};

    struct wl_display* wl_display_{nullptr};
    struct wl_event_loop* wl_event_loop_{nullptr};
    struct wlr_backend* backend_{nullptr};
    struct wlr_renderer* renderer_{nullptr};
    struct wlr_allocator* allocator_{nullptr};
    struct wlr_compositor* wlr_compositor_{nullptr};
    struct wlr_subcompositor* subcompositor_{nullptr};
    struct wlr_output_layout* output_layout_{nullptr};
    struct wlr_cursor* cursor_{nullptr};
    struct wlr_xcursor_manager* cursor_mgr_{nullptr};
    struct wlr_seat* seat_{nullptr};
    struct wlr_xdg_shell* xdg_shell_{nullptr};

    // Hardware GPU wlr_scene graph trees
    struct wlr_scene* scene_{nullptr};
    struct wlr_scene_tree* background_tree_{nullptr};
    struct wlr_scene_tree* windows_tree_{nullptr};
    struct wlr_scene_tree* chrome_tree_{nullptr};
    struct wlr_scene_tree* hud_tree_{nullptr};

    // GPU-native geometry nodes
    struct wlr_scene_rect* wallpaper_rect_{nullptr};
    struct wlr_scene_rect* bg_sky_mid_{nullptr};
    struct wlr_scene_rect* bg_sunset_glow_{nullptr};
    struct wlr_scene_rect* bg_sunset_horizon_{nullptr};
    struct wlr_scene_rect* bg_silhouette_{nullptr};
    struct wlr_scene_rect* bg_water_glow_{nullptr};

    struct wlr_scene_rect* top_bar_rect_{nullptr};
    struct wlr_scene_rect* top_bar_border_{nullptr};
    struct wlr_scene_rect* top_bar_icon_{nullptr};
    struct wlr_scene_rect* top_bar_clock_pill_{nullptr};
    struct wlr_scene_rect* top_bar_clock_handle_{nullptr};
    struct wlr_scene_rect* top_bar_wifi_pill_{nullptr};
    struct wlr_scene_rect* top_bar_battery_pill_{nullptr};
    struct wlr_scene_rect* top_bar_bell_pill_{nullptr};

    struct wlr_scene_rect* split_divider_line_{nullptr};
    struct wlr_scene_rect* split_divider_pill_{nullptr};

    struct wlr_scene_rect* dock_border_rect_{nullptr};
    struct wlr_scene_rect* dock_bg_rect_{nullptr};
    struct wlr_scene_rect* dock_launcher_card_{nullptr};
    std::vector<struct wlr_scene_rect*> dock_launcher_tiles_;
    struct wlr_scene_rect* dock_separator_{nullptr};
    std::vector<struct wlr_scene_rect*> dock_icon_rects_;
    std::vector<struct wlr_scene_rect*> dock_active_dots_;
    struct wlr_scene_rect* dock_active_dot_{nullptr};

    struct wlr_scene_rect* hud_bg_rect_{nullptr};
    struct wlr_scene_rect* hud_border_rect_{nullptr};
    struct wlr_scene_rect* hud_status_pill_{nullptr};

    // Hardware Pixel Buffers for full resolution image & font rendering
    struct wlr_scene_buffer* wallpaper_scene_buf_{nullptr};
    struct wlr_scene_buffer* top_bar_scene_buf_{nullptr};
    struct wlr_scene_buffer* dock_scene_buf_{nullptr};
    std::unique_ptr<render::FrameBuffer> wallpaper_fb_;
    int last_scene_w_{0};
    int last_scene_h_{0};
    int last_clock_sec_{-1};

    std::shared_ptr<decoration::TilingDecorationSpec> decoration_spec_;
    std::unique_ptr<decoration::TilingDragManager> drag_manager_;

    std::vector<std::unique_ptr<WlrOutput>> outputs_;
    uint64_t last_frame_time_ns_{0};
    float current_fps_{0.0f};
    uint64_t frame_count_{0};

    std::unique_ptr<ipc::IpcServer> ipc_server_;
};

} // namespace prism::wm
