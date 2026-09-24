#pragma once

#include "prism/wm/window.hpp"
#include "prism/layout/layout_strategy.hpp"
#include "prism/core/types.hpp"
#include "prism/tree/tree_engine.hpp"
#include "prism/decoration/tiling_decoration_spec.hpp"
#include <vector>
#include <memory>
#include <string>

namespace prism::wm {

class Compositor {
public:
    Compositor();
    ~Compositor();

    bool Initialize();

    // Factory method for creating and managing a new Prism window
    std::shared_ptr<Window> CreateWindow(
        const std::string& app_id,
        const std::string& title,
        core::Rect bounds,
        const std::string& channel_name
    );

    // Swap layout strategy dynamically (Strategy Pattern)
    void SetLayoutStrategy(std::unique_ptr<layout::LayoutStrategy> strategy);
    layout::LayoutStrategy* GetLayoutStrategy() const { return layout_strategy_.get(); }

    // Frame execution
    void Tick(float dt);
    void Render();
    void RenderToFrameBuffer(render::FrameBuffer& fb);

    // User input event injection & distribution
    void DispatchAction(const std::string& app_id, const std::string& action);

    // Pointer and input events (forwarded from wlroots wlr_cursor / wlr_seat or simulated)
    void OnPointerMotion(float x, float y, float dx = 0.0f, float dy = 0.0f);
    void OnPointerButton(uint32_t button, bool pressed);
    void OnGesture(core::GestureType gesture, float val = 0.0f);

    float GetCursorX() const { return cursor_x_; }
    float GetCursorY() const { return cursor_y_; }

    void InjectPointerMotion(float x, float y) { OnPointerMotion(x, y); }
    void InjectPointerButton(uint32_t button, bool pressed) { OnPointerButton(button, pressed); }
    void InjectGesture(core::GestureType gesture, float val = 0.0f) { OnGesture(gesture, val); }

    // macOS Mission Control Overview
    void ToggleMissionControl();
    void SetMissionControl(bool enabled);
    bool IsInMissionControl() const;

    int GetFocusedWindowIndex() const { return focused_window_index_; }
    void SetFocusedWindowIndex(int idx);

    void DestroyWindow(const std::shared_ptr<Window>& win);
    const std::vector<std::shared_ptr<Window>>& GetWindows() const { return windows_; }

    // Multi-Level Recursive BSP Tree Engine
    tree::TreeEngine& GetTreeEngine() { return tree_engine_; }
    const tree::TreeEngine& GetTreeEngine() const { return tree_engine_; }

    void SetDecorationSpec(std::shared_ptr<decoration::TilingDecorationSpec> spec) { decoration_spec_ = std::move(spec); }
    std::shared_ptr<decoration::TilingDecorationSpec> GetDecorationSpec() const { return decoration_spec_; }

    bool MoveFocus(tree::Direction dir);
    bool SwitchWorkspace(const std::string& name);
    bool SetTreeLayout(tree::LayoutMode mode);
    bool SwapFocusDirection(tree::Direction dir);

    void SetDrawSoftwareCursor(bool draw) { draw_software_cursor_ = draw; }
    bool GetDrawSoftwareCursor() const { return draw_software_cursor_; }

    void SetDebugHud(bool enable) { debug_hud_enabled_ = enable; }
    bool IsDebugHudEnabled() const { return debug_hud_enabled_; }

    void SetPerformanceStats(float fps, float dt, uint64_t frame_count) {
        last_fps_ = fps;
        last_dt_ = dt;
        frame_count_ = frame_count;
    }
    float GetLastFps() const { return last_fps_; }
    float GetLastDt() const { return last_dt_; }

    void SetScreenSize(int w, int h) { screen_width_ = w; screen_height_ = h; }
    int GetScreenWidth() const { return screen_width_; }
    int GetScreenHeight() const { return screen_height_; }

private:
    std::vector<std::shared_ptr<Window>> windows_;
    std::unique_ptr<layout::LayoutStrategy> layout_strategy_;
    tree::TreeEngine tree_engine_;
    std::shared_ptr<decoration::TilingDecorationSpec> decoration_spec_;
    float cursor_x_{960.0f};
    float cursor_y_{540.0f};
    bool is_dragging_divider_{false};
    int focused_window_index_{0};
    bool running_{false};
    bool draw_software_cursor_{false};
    bool debug_hud_enabled_{true};
    float last_fps_{0.0f};
    float last_dt_{0.016f};
    uint64_t frame_count_{0};
    int screen_width_{1920};
    int screen_height_{1080};
};

} // namespace prism::wm
