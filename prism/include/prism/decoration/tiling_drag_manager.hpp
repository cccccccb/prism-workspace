#pragma once

#include "prism/core/types.hpp"
#include "prism/decoration/tiling_decoration_spec.hpp"
#include <memory>
#include <vector>

struct wlr_scene_tree;
struct wlr_scene_rect;

namespace prism::wm {
class Window;
}

namespace prism::decoration {

enum class DropQuadrant {
    None,
    Swap,       // Center: swap window positions
    LeftSplit,  // Left 25%: insert split on the left
    RightSplit, // Right 25%: insert split on the right
    TopSplit,   // Top 25%: insert split on top
    BottomSplit // Bottom 25%: insert split at the bottom
};

struct DragDropResult {
    bool executed{false};
    std::shared_ptr<wm::Window> source_window;
    std::shared_ptr<wm::Window> target_window;
    DropQuadrant quadrant{DropQuadrant::None};
};

class TilingDragManager {
public:
    explicit TilingDragManager(std::shared_ptr<TilingDecorationSpec> spec = nullptr);
    ~TilingDragManager();

    // Attach drop-zone preview scene nodes to the compositor overlay layer
    void AttachToScene(struct wlr_scene_tree *overlay_tree);

    // Lifecycle of a titlebar drag session
    bool BeginDrag(std::shared_ptr<wm::Window> window, float start_x, float start_y);
    void UpdateDrag(float current_x, float current_y,
                    const std::vector<std::shared_ptr<wm::Window>> &all_windows);
    DragDropResult EndDrag();
    void CancelDrag();

    bool IsDragging() const
    {
        return is_dragging_;
    }

    std::shared_ptr<wm::Window> GetDraggedWindow() const
    {
        return dragged_window_;
    }

    DropQuadrant GetCurrentQuadrant() const
    {
        return current_quadrant_;
    }

    core::Rect GetPreviewBounds() const
    {
        return preview_bounds_;
    }

private:
    void UpdateDropIndicatorScene();
    void HideDropIndicator();

    std::shared_ptr<TilingDecorationSpec> spec_;
    bool is_dragging_{false};
    float start_x_{0.0f};
    float start_y_{0.0f};
    float current_x_{0.0f};
    float current_y_{0.0f};

    std::shared_ptr<wm::Window> dragged_window_{nullptr};
    std::shared_ptr<wm::Window> target_window_{nullptr};
    DropQuadrant current_quadrant_{DropQuadrant::None};
    core::Rect preview_bounds_{0, 0, 0, 0};

    // wlroots scene overlay nodes for Drop-Zone visualization
    struct wlr_scene_tree *overlay_tree_{nullptr};
    struct wlr_scene_tree *drop_zone_tree_{nullptr};
    struct wlr_scene_rect *drop_zone_fill_{nullptr};
    struct wlr_scene_rect *drop_border_top_{nullptr};
    struct wlr_scene_rect *drop_border_bottom_{nullptr};
    struct wlr_scene_rect *drop_border_left_{nullptr};
    struct wlr_scene_rect *drop_border_right_{nullptr};
};

} // namespace prism::decoration
