#pragma once

#include "prism/core/noncopyable.hpp"
#include "prism/core/types.hpp"
#include "prism/decoration/tiling_drag_manager.hpp"
#include "prism/tree/tree_constraints.hpp"
#include "prism/wm/compositor.hpp"
#include "prism/wm/layout_control.hpp"
#include "prism/wm/performance.hpp"
#include "prism/wm/surface_effects.hpp"
#include "prism/wm/theme.hpp"

#include "prism/launch/control_protocol.hpp"
#include "prism/launch/stream.hpp"
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <sys/types.h>
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
struct wlr_xdg_toplevel;
struct wlr_xdg_popup;
struct wlr_seat;
struct wlr_cursor;
struct wlr_xcursor_manager;
struct wlr_output;
struct wlr_input_device;
struct wlr_keyboard;
struct wlr_touch;
struct wlr_touch_down_event;
struct wlr_touch_motion_event;
struct wlr_seat_client;
struct wlr_virtual_keyboard_manager_v1;
struct wlr_virtual_keyboard_v1;
struct wlr_virtual_pointer_manager_v1;
struct wlr_virtual_pointer_v1_new_pointer_event;
struct wlr_surface;

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
struct WlrXdgView;
struct WlrXdgPopup;
struct WlrKeyboardBinding;
struct WlrTouchBinding;
struct WlrPointerBinding;
class SurfaceEffects;
class SurfaceFade;
class SurfaceGeometry;
class GroupSurfaceGeometry;
struct WlrSurfaceWatch;

struct FrameWorkCounters {
    std::uint64_t frame_events{}, idle_skips{}, scene_commit_calls{}, scene_commit_noops{};
    std::uint64_t output_commits{}, output_buffer_commits{}, frame_done_dispatches{};
    std::uint64_t needs_frame_events{}, damage_events{};
    std::uint64_t layout_requests{}, effects_requests{}, mode_requests{};
    std::uint64_t surface_commits{}, surface_buffer_commits{}, surface_callback_commits{},
        surface_nonvisual_commits{};
};

struct WlrOutput {
    WlrOutput(struct wlr_output *out, WlrServer *server);
    ~WlrOutput();

    struct wlr_output *wlr_output{nullptr};
    struct wlr_scene_output *scene_output{nullptr};
    WlrServer *server{nullptr};
    struct wl_listener frame;
    struct wl_listener present;
    struct wl_listener committed, needs_frame, damage;
    std::uint64_t last_present_ns{}, presented_count{}, discarded_count{};
    TimingSamples present_intervals;
    std::uint64_t last_frame_ns{};
    std::uint64_t layout_id{};
    struct wl_listener request_state;
    struct wl_listener destroy;
};

struct WlrServerSignals {
    struct wl_listener output_layout_change;
    struct wl_listener new_output;
    struct wl_listener new_input;
    struct wl_listener new_virtual_keyboard;
    struct wl_listener new_virtual_pointer;
    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_xdg_popup;
    struct wl_listener new_surface;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;
    WlrServer *server{nullptr};
};

/**
 * @brief Wayland & wlroots Compositor Server Engine (Sway architecture)
 *        100% Native GPU Hardware Compositing via wlr_scene.
 *        Manages display lifecycle, multi-output DRM/KMS scanout, input seats,
 *        hardware cursors, and sub-millisecond GPU scene presentation.
 */
class WlrServer : public core::NonCopyable, private LayoutControlApplier {
public:
    explicit WlrServer(std::shared_ptr<Compositor> compositor);
    ~WlrServer();

    bool Initialize(const std::string &socket_name = "");
    void Start();
    void AttachControl(int fd, int parent_pid);

    bool ControlHealthy() const
    {
        return !control_failed_;
    }

    // Called on the Wayland event loop. Validation precedes all state changes.
    contracts::ThemeApplied InstallTheme(const contracts::ThemeSnapshot &theme);

    // Event-loop thread only. Values never retain mutable tree/window pointers.
    std::shared_ptr<const contracts::LayoutSnapshot> GetLayoutSnapshot();
    void InvalidateLayoutSnapshot() noexcept;

    const contracts::ThemeSnapshot *GetTheme() const
    {
        return theme_snapshot_.get();
    }

    void Stop();

    void RunEventLoopIteration(int timeout_ms = 0);

    struct wl_display *GetDisplay() const
    {
        return wl_display_;
    }

    struct wl_event_loop *GetEventLoop() const
    {
        return wl_event_loop_;
    }

    struct wlr_backend *GetBackend() const
    {
        return backend_;
    }

    struct wlr_renderer *GetRenderer() const
    {
        return renderer_;
    }

    struct wlr_output_layout *GetOutputLayout() const
    {
        return output_layout_;
    }

    struct wlr_scene *GetScene() const
    {
        return scene_;
    }

    struct wlr_cursor *GetCursor() const
    {
        return cursor_;
    }

    struct wlr_seat *GetSeat() const
    {
        return seat_;
    }

    const std::string &GetSocketName() const
    {
        return socket_name_;
    }

    bool IsRunning() const
    {
        return running_;
    }

    void HandleNewOutput(struct wlr_output *output);
    void HandleNewInput(struct wlr_input_device *device);
    void HandleNewVirtualKeyboard(struct wlr_virtual_keyboard_v1 *keyboard);
    void HandleNewVirtualPointer(struct wlr_virtual_pointer_v1_new_pointer_event *event);
    void HandleNewXdgToplevel(struct wlr_xdg_toplevel *toplevel);
    void HandleNewXdgPopup(struct wlr_xdg_popup *popup);
    void HandleXdgMap(WlrXdgView *view);
    void HandleXdgCommit(WlrXdgView *view);
    void HandleXdgUnmap(WlrXdgView *view);
    void HandleXdgDestroy(WlrXdgView *view);
    void HandleXdgMaximize(WlrXdgView *view);
    void HandleKeyboardKey(WlrKeyboardBinding *binding, void *event);
    void HandleKeyboardModifiers(WlrKeyboardBinding *binding);
    void HandleKeyboardKeymap(WlrKeyboardBinding *binding);
    void HandleKeyboardDestroy(WlrKeyboardBinding *binding);
    void HandleTouchDown(WlrTouchBinding *, const wlr_touch_down_event &);
    void HandleTouchMotion(WlrTouchBinding *, const wlr_touch_motion_event &);
    void HandleTouchUp(WlrTouchBinding *, std::uint32_t time_msec, std::int32_t contact);
    void HandleTouchCancel(WlrTouchBinding *, std::int32_t contact);
    void HandleTouchFrame();
    void HandleTouchDestroy(WlrTouchBinding *);
    void CloseFocusedXdgView();
    void HandleCursorMotion(uint32_t time_msec, double dx, double dy,
                            struct wlr_input_device *device = nullptr);
    void HandleCursorMotionAbsolute(uint32_t time_msec, double x, double y,
                                    struct wlr_input_device *device = nullptr);
    void HandleCursorButton(uint32_t time_msec, uint32_t button, uint32_t state,
                            struct wlr_input_device *device = nullptr);
    void HandlePointerDestroy(WlrPointerBinding *);
    void HandleCursorAxis(uint32_t time_msec, int axis, double value, int32_t discrete, int source,
                          int relative_direction);
    void HandleOutputFrame(WlrOutput *output);
    void HandleOutputCommit(const void *event);

    void HandleOutputNeedsFrame()
    {
        ++frame_work_.needs_frame_events;
    }

    void HandleOutputDamage()
    {
        ++frame_work_.damage_events;
    }

    void HandleNewSurface(wlr_surface *surface);
    void HandleSurfaceCommit(wlr_surface *surface);
    void HandleSurfaceMapState(wlr_surface *surface);
    void HandleSurfaceDestroy(wlr_surface *surface);
    void RemoveOutput(WlrOutput *output);

    std::shared_ptr<Compositor> GetCompositor() const
    {
        return compositor_;
    }

    // Dynamic Display & Mode control (Sway architecture)
    bool SetOutputMode(const std::string &name, int width, int height, int refresh_mhz = 0);
    bool SetAdaptiveSync(const std::string &name, bool enabled);
    std::vector<OutputInfo> GetOutputsInfo() const;

    float GetCurrentFps() const
    {
        return current_fps_;
    }

    uint64_t GetFrameCount() const
    {
        return frame_count_;
    }

    ipc::IpcServer *GetIpcServer() const
    {
        return ipc_server_.get();
    }

private:
    static bool FilterGlobal(const struct wl_client *client, const struct wl_global *global,
                             void *data);
    void InitializeIpc();
    std::string IpcOutputs(const std::string &, const std::vector<std::string> &);
    std::string IpcMode(const std::string &, const std::vector<std::string> &);
    std::string IpcStatus(const std::string &, const std::vector<std::string> &);
    std::string IpcDebug(const std::string &, const std::vector<std::string> &);
    std::string IpcLayout(const std::string &, const std::vector<std::string> &);
    std::string IpcFocus(const std::string &, const std::vector<std::string> &);
    std::string IpcSwap(const std::string &, const std::vector<std::string> &);
    std::string IpcWorkspace(const std::string &, const std::vector<std::string> &);
    std::string IpcMoveWorkspace(const std::string &, const std::vector<std::string> &);
    std::string IpcTree(const std::string &, const std::vector<std::string> &);
    std::string IpcClose(const std::string &, const std::vector<std::string> &);
    std::string IpcTheme(const std::string &, const std::vector<std::string> &);
    std::string IpcFold(const std::string &, const std::vector<std::string> &);
    std::string IpcFullscreen(const std::string &, const std::vector<std::string> &);
    std::string IpcAction(const std::string &, const std::vector<std::string> &);
    std::string IpcBenchmark(const std::string &, const std::vector<std::string> &);

    void HandleEffectsWake();
    static void HandleEffectsIdle(void *data);
    void InitSceneGraph();
    void UpdateSceneGraph(int width, int height, float dt = 0.016f);
    void FocusXdgView(WlrXdgView *view);
    void ArrangeXdgViews();
    void SynchronizeXdgFocus();
    void SetXdgFullscreen(WlrXdgView *view, bool enabled);
    friend struct WlrXdgPopup;
    WlrXdgView *XdgOwner(wlr_surface *surface) const;
    WlrXdgPopup *FindXdgPopup(wlr_surface *surface) const;
    void ConfigureXdgPopup(WlrXdgPopup *popup);
    void DismissXdgPopup(WlrXdgPopup *popup);
    void RemoveXdgPopup(WlrXdgPopup *popup);
    void CloseXdgPopups(WlrXdgView *owner = nullptr);
    void RestorePopupFocus();
    void UpdateXdgPointerFocus(uint32_t time_msec);
    void AttachTouchDevice(wlr_touch *);
    void CancelTouchDevice(WlrTouchBinding *);
    void CancelTouchClient(wlr_seat_client *);
    void CancelTouchesForSurface(wlr_surface *);
    std::int32_t AllocateTouchId();
    enum class FrameReason { Layout, Effects, Mode };
    void ScheduleFrames(FrameReason reason);
    void InvalidateEffects();
    std::vector<SurfaceEffects::Target> PopupEffectTargets() const;
    bool UpdateSurfaceEffects();

    WlrServerSignals signals_{};
    std::shared_ptr<Compositor> compositor_;
    std::string socket_name_;
    bool running_{false};

    struct wl_display *wl_display_{nullptr};
    struct wl_event_loop *wl_event_loop_{nullptr};
    struct wl_event_source *effects_idle_{nullptr};
    struct wlr_backend *backend_{nullptr};
    struct wlr_renderer *renderer_{nullptr};
    struct wlr_allocator *allocator_{nullptr};
    struct wlr_compositor *wlr_compositor_{nullptr};
    struct wlr_subcompositor *subcompositor_{nullptr};
    struct wlr_output_layout *output_layout_{nullptr};
    struct wlr_cursor *cursor_{nullptr};
    struct wlr_xcursor_manager *cursor_mgr_{nullptr};
    struct wlr_seat *seat_{nullptr};
    struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_manager_{nullptr};
    struct wlr_virtual_pointer_manager_v1 *virtual_pointer_manager_{nullptr};
    struct wlr_xdg_shell *xdg_shell_{nullptr};

    // Hardware GPU wlr_scene graph trees
    struct wlr_scene *scene_{nullptr};
    struct wlr_scene_tree *background_tree_{nullptr};
    struct wlr_scene_tree *windows_tree_{nullptr};
    struct wlr_scene_tree *chrome_tree_{nullptr};
    struct wlr_scene_tree *hud_tree_{nullptr};

    // GPU-native geometry nodes
    struct wlr_scene_rect *wallpaper_rect_{nullptr};
    struct wlr_scene_rect *bg_sky_mid_{nullptr};
    struct wlr_scene_rect *bg_sunset_glow_{nullptr};
    struct wlr_scene_rect *bg_sunset_horizon_{nullptr};
    struct wlr_scene_rect *bg_silhouette_{nullptr};
    struct wlr_scene_rect *bg_water_glow_{nullptr};

    struct wlr_scene_rect *top_bar_rect_{nullptr};
    struct wlr_scene_rect *top_bar_border_{nullptr};
    struct wlr_scene_rect *top_bar_icon_{nullptr};
    struct wlr_scene_rect *top_bar_clock_pill_{nullptr};
    struct wlr_scene_rect *top_bar_clock_handle_{nullptr};
    struct wlr_scene_rect *top_bar_wifi_pill_{nullptr};
    struct wlr_scene_rect *top_bar_battery_pill_{nullptr};
    struct wlr_scene_rect *top_bar_bell_pill_{nullptr};

    struct wlr_scene_rect *split_divider_line_{nullptr};
    struct wlr_scene_rect *split_divider_pill_{nullptr};

    struct wlr_scene_rect *dock_border_rect_{nullptr};
    struct wlr_scene_rect *dock_bg_rect_{nullptr};
    struct wlr_scene_rect *dock_launcher_card_{nullptr};
    std::vector<struct wlr_scene_rect *> dock_launcher_tiles_;
    struct wlr_scene_rect *dock_separator_{nullptr};
    std::vector<struct wlr_scene_rect *> dock_icon_rects_;
    std::vector<struct wlr_scene_rect *> dock_active_dots_;
    struct wlr_scene_rect *dock_active_dot_{nullptr};

    struct wlr_scene_rect *hud_bg_rect_{nullptr};
    struct wlr_scene_rect *hud_border_rect_{nullptr};
    struct wlr_scene_rect *hud_status_pill_{nullptr};

    std::unique_ptr<decoration::TilingDragManager> drag_manager_;

    std::vector<std::unique_ptr<WlrOutput>> outputs_;
    std::vector<std::unique_ptr<WlrXdgView>> xdg_views_;
    std::vector<std::unique_ptr<WlrXdgPopup>> xdg_popups_;
    std::unique_ptr<SurfaceEffects> surface_effects_;
    std::map<wlr_surface *, std::unique_ptr<WlrSurfaceWatch>> surface_watches_;
    FrameWorkCounters frame_work_;

    struct Registration {
        launch::ShellPermit permit;
        int pidfd{-1};
        bool consumed{};
        std::unique_ptr<launch::ShellPermitGuard> guard;
        ~Registration();
    };

    void PumpControl();
    void PublishLayoutSnapshot();
    LayoutControlPrincipal LayoutPrincipal(const launch::ShellPermit &) const;
    void HandleLayoutControl(const launch::ControlMessage &);
    void PublishLayoutControlNotifications();
    void RecordLayoutInput(wlr_surface *, contracts::LayoutInputProof);
    void CancelLayoutControlsForSurface(
        wlr_surface *,
        contracts::LayoutControlError error = contracts::LayoutControlError::InvalidInput);
    contracts::LayoutControlError ApplyLayoutIntent(const contracts::LayoutControlRequest &,
                                                    contracts::LayoutControlResult &) override;
    contracts::LayoutControlError TrackLayoutIntent(const contracts::LayoutControlRequest &,
                                                    contracts::LayoutControlResult &) override;
    contracts::LayoutControlError ApplyBoundaryIntent(const contracts::LayoutControlRequest &,
                                                      contracts::LayoutControlResult &);
    void CancelBoundaryPreview();
    void UpdateBoundaryControl();
    void SampleBoundaryPointer();
    bool ConsumeWindowPointer(std::uint32_t button, std::uint32_t state, wlr_input_device *);
    void CloseWindowControl(bool animate = false);
    bool UpdateWindowControl();
    WlrXdgView *WindowControlTarget() const;
    contracts::LayoutControlError TrackWindowIntent(const contracts::LayoutControlRequest &) const;
    contracts::LayoutControlError ApplyWindowIntent(const contracts::LayoutControlRequest &,
                                                    contracts::LayoutControlResult &);
    WlrXdgView *BoundaryControlView() const;
    void SubmitXdgSize(WlrXdgView *);
    tree::TreeLayoutConfig CurrentTreeLayout() const;
    void ReconcileGroupModes();
    bool GroupImmersive() const;
    bool GroupControlsHidden() const;
    bool SetGroupMode(std::uint64_t workspace, contracts::LayoutGroupMode);
    bool ClearGroupFullscreen(std::uint64_t workspace);
    void RestoreDesktopGroup(bool all = false);
    void ArrangeGroupTransition();
    void HandleShellUnavailable(int role);
    WlrXdgView *RecoveryTopbar() const;
    bool ConsumeGroupPointer(std::uint32_t button, std::uint32_t state, wlr_input_device *device);
    void UpdateGroupRecovery();
    bool ConstrainRecoveryFocus(wlr_surface *surface, double x, double y) const;
    core::Rect PrimaryLogicalBounds() const;
    static void HandleOutputLayoutChange(wl_listener *, void *);
    void NotifyView(WlrXdgView *view, launch::ControlType type);
    std::unique_ptr<launch::Stream> control_;
    std::map<pid_t, std::unique_ptr<Registration>> registrations_;
    std::uint64_t control_session_{};
    bool control_failed_{};
    std::shared_ptr<const contracts::LayoutSnapshot> layout_snapshot_;
    std::shared_ptr<const tree::TreeSnapshot> layout_tree_snapshot_;
    std::uint64_t layout_theme_generation_{}, layout_sent_revision_{}, next_output_id_{1};
    std::uint64_t layout_constraints_generation_{}, layout_observed_constraints_generation_{};
    bool layout_snapshot_dirty_{true}, layout_subscribed_{};
    LayoutControlAuthority layout_controls_;

    struct BoundaryPointer {
        contracts::LayoutInputProof proof;
        std::uint64_t boundary{};
        contracts::LogicalPoint start, position;
        double divider_position{};
        bool released{};
    };

    struct BoundaryDrag {
        std::uint64_t session{};
        tree::BoundaryFractions original, last;
    };

    struct WindowControl {
        std::uint64_t node{}, topology{}, layout{}, session{}, proof_node{};
        contracts::LogicalRect bounds;
        wlr_input_device *device{};
        contracts::LayoutInputProof proof;
        contracts::LogicalPoint release;
        bool released{};
    };

    WindowControl window_control_;
    std::unique_ptr<SurfaceFade> control_fade_;
    std::unique_ptr<SurfaceGeometry> surface_geometry_;
    std::unique_ptr<GroupSurfaceGeometry> group_geometry_;
    bool arranging_group_transition_{};
    std::optional<BoundaryPointer> boundary_pointer_;
    std::optional<BoundaryDrag> boundary_drag_;
    contracts::LayoutControlHandle boundary_handle_;

    struct GroupState {
        std::uint64_t output{}, revision{1};
        contracts::LayoutGroupMode mode{contracts::LayoutGroupMode::Normal};
    };

    std::map<std::uint64_t, GroupState> group_modes_;
    std::uint64_t recovery_workspace_{}, recovery_output_{};
    core::Rect recovery_output_bounds_{};
    bool recovery_visible_{};
    // A system-owned Down keeps all subsequent chord buttons until their Up,
    // even when the workspace/output changes while the button is held.
    std::set<std::pair<wlr_input_device *, std::uint32_t>> recovery_buttons_;
    std::vector<std::unique_ptr<WlrPointerBinding>> pointers_;
    wlr_input_device *control_pointer_device_{};
    std::vector<std::unique_ptr<WlrKeyboardBinding>> keyboards_;
    std::vector<std::unique_ptr<WlrTouchBinding>> touches_;
    std::int32_t next_touch_id_{};
    WlrXdgView *focused_xdg_view_{nullptr};
    WlrXdgView *dragged_xdg_view_{nullptr};
    ThemeGeometry theme_{};
    std::shared_ptr<const contracts::ThemeSnapshot> theme_snapshot_;
    uint64_t last_frame_time_ns_{0};
    float current_fps_{0.0f};
    uint64_t frame_count_{0};
    std::uint64_t commit_successes_{}, commit_failures_{}, pointer_events_{};
    TimingSamples frame_cpu_, effects_cpu_, commit_cpu_, pointer_event_age_;

    std::unique_ptr<ipc::IpcServer> ipc_server_;
};

} // namespace prism::wm
