#pragma once

#include "prism/contracts/events.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <wayland-client.h>

struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_shm;
struct wl_seat;
struct wl_pointer;
struct wl_keyboard;
struct wl_surface;
struct wl_callback;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;

namespace prism::platform {

// A single xdg-shell toplevel. It owns Wayland objects and temporary SHM
// buffers; the DSL and renderer do not depend on these implementation types.
class WaylandWindow {
public:
    WaylandWindow();
    ~WaylandWindow();
    WaylandWindow(const WaylandWindow&) = delete;
    WaylandWindow& operator=(const WaylandWindow&) = delete;

    bool Open(const std::string& socket_name, const std::string& app_id,
              const std::string& title, int preferred_width, int preferred_height);
    void SetEventHandler(std::function<void(const contracts::WindowEvent&)> handler) {
        event_handler_ = std::move(handler);
    }
    void SetPaintHandler(std::function<void(void*, int, int, int)> handler) {
        paint_handler_ = std::move(handler);
    }
    void RequestRedraw();
    bool Pump(int timeout_ms);
    void RequestMaximize();
    void Close();

    bool IsConfigured() const { return configured_; }
    bool IsMapped() const { return mapped_; }
    bool IsCloseRequested() const { return close_requested_; }
    int ConfigureCount() const { return configure_count_; }
    int FrameDoneCount() const { return frame_done_count_; }
    int PointerEnterCount() const { return pointer_enter_count_; }
    int PointerButtonCount() const { return pointer_button_count_; }
    int KeyCount() const { return key_count_; }
    contracts::WindowMetrics Metrics() const { return metrics_; }

private:
    struct ShmBuffer;
    static void RegistryGlobal(void*, wl_registry*, std::uint32_t, const char*, std::uint32_t);
    static void RegistryGlobalRemove(void*, wl_registry*, std::uint32_t);
    static void ShellPing(void*, xdg_wm_base*, std::uint32_t);
    static void SurfaceConfigure(void*, xdg_surface*, std::uint32_t);
    static void ToplevelConfigure(void*, xdg_toplevel*, std::int32_t, std::int32_t, wl_array*);
    static void ToplevelClose(void*, xdg_toplevel*);
    static void ToplevelConfigureBounds(void*, xdg_toplevel*, std::int32_t, std::int32_t);
    static void ToplevelCapabilities(void*, xdg_toplevel*, wl_array*);
    static void SeatCapabilities(void*, wl_seat*, std::uint32_t);
    static void SeatName(void*, wl_seat*, const char*);
    static void PointerEnter(void*, wl_pointer*, std::uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t);
    static void PointerLeave(void*, wl_pointer*, std::uint32_t, wl_surface*);
    static void PointerMotion(void*, wl_pointer*, std::uint32_t, wl_fixed_t, wl_fixed_t);
    static void PointerButton(void*, wl_pointer*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
    static void PointerAxis(void*, wl_pointer*, std::uint32_t, std::uint32_t, wl_fixed_t);
    static void PointerFrame(void*, wl_pointer*);
    static void PointerAxisSource(void*, wl_pointer*, std::uint32_t);
    static void PointerAxisStop(void*, wl_pointer*, std::uint32_t, std::uint32_t);
    static void PointerAxisDiscrete(void*, wl_pointer*, std::uint32_t, std::int32_t);
    static void PointerAxisValue120(void*, wl_pointer*, std::uint32_t, std::int32_t);
    static void PointerAxisRelativeDirection(void*, wl_pointer*, std::uint32_t, std::uint32_t);
    static void KeyboardKeymap(void*, wl_keyboard*, std::uint32_t, int, std::uint32_t);
    static void KeyboardEnter(void*, wl_keyboard*, std::uint32_t, wl_surface*, wl_array*);
    static void KeyboardLeave(void*, wl_keyboard*, std::uint32_t, wl_surface*);
    static void KeyboardKey(void*, wl_keyboard*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
    static void KeyboardModifiers(void*, wl_keyboard*, std::uint32_t, std::uint32_t,
                                  std::uint32_t, std::uint32_t, std::uint32_t);
    static void KeyboardRepeatInfo(void*, wl_keyboard*, std::int32_t, std::int32_t);
    static void FrameDone(void*, wl_callback*, std::uint32_t);
    static void BufferRelease(void*, wl_buffer*);

    ShmBuffer* AcquireBuffer();
    void Emit(contracts::WindowEvent event);
    void TryRender();
    void ReapBuffers();

    wl_display* display_{nullptr};
    wl_registry* registry_{nullptr};
    wl_compositor* compositor_{nullptr};
    wl_shm* shm_{nullptr};
    wl_seat* seat_{nullptr};
    std::uint32_t seat_global_name_{0};
    wl_pointer* pointer_{nullptr};
    wl_keyboard* keyboard_{nullptr};
    xdg_wm_base* shell_{nullptr};
    wl_surface* surface_{nullptr};
    xdg_surface* xdg_surface_{nullptr};
    xdg_toplevel* toplevel_{nullptr};
    wl_callback* frame_callback_{nullptr};
    std::vector<std::unique_ptr<ShmBuffer>> buffers_;
    std::function<void(const contracts::WindowEvent&)> event_handler_;
    std::function<void(void*, int, int, int)> paint_handler_;
    contracts::WindowMetrics metrics_{};
    contracts::LogicalPoint pointer_position_{};
    int preferred_width_{640};
    int preferred_height_{400};
    int pending_width_{0};
    int pending_height_{0};
    int configure_count_{0};
    int frame_done_count_{0};
    int pointer_enter_count_{0};
    int pointer_button_count_{0};
    int key_count_{0};
    bool configured_{false};
    bool mapped_{false};
    bool dirty_{false};
    bool close_requested_{false};
};

} // namespace prism::platform
