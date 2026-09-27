#include "prism/decoration/tiling_drag_manager.hpp"
#include "prism/core/logging.hpp"
#include "prism/wm/window.hpp"
#include <algorithm>

extern "C" {
#include <wlr/types/wlr_scene.h>
}

namespace prism::decoration {

static inline void ColorToFloat4(const core::Color &c, float out[4])
{
    out[0] = static_cast<float>(c.r) / 255.0f;
    out[1] = static_cast<float>(c.g) / 255.0f;
    out[2] = static_cast<float>(c.b) / 255.0f;
    out[3] = static_cast<float>(c.a) / 255.0f;
}

TilingDragManager::TilingDragManager(std::shared_ptr<TilingDecorationSpec> spec)
    : spec_(spec ? spec : TilingDecorationSpec::CreateDefault())
{
}

TilingDragManager::~TilingDragManager()
{
    if (drop_zone_tree_) {
        wlr_scene_node_destroy(&drop_zone_tree_->node);
        drop_zone_tree_ = nullptr;
    }
}

void TilingDragManager::AttachToScene(struct wlr_scene_tree *overlay_tree)
{
    if (!overlay_tree) {
        return;
    }
    overlay_tree_ = overlay_tree;

    drop_zone_tree_ = wlr_scene_tree_create(overlay_tree_);

    float fill_col[4];
    ColorToFloat4(spec_->drop_zone.fill_color, fill_col);
    drop_zone_fill_ = wlr_scene_rect_create(drop_zone_tree_, 100, 100, fill_col);

    float border_col[4];
    ColorToFloat4(spec_->drop_zone.border_color, border_col);
    int bw = static_cast<int>(spec_->drop_zone.border_width);
    drop_border_top_ = wlr_scene_rect_create(drop_zone_tree_, 100, bw, border_col);
    drop_border_bottom_ = wlr_scene_rect_create(drop_zone_tree_, 100, bw, border_col);
    drop_border_left_ = wlr_scene_rect_create(drop_zone_tree_, bw, 100, border_col);
    drop_border_right_ = wlr_scene_rect_create(drop_zone_tree_, bw, 100, border_col);

    HideDropIndicator();
}

bool TilingDragManager::BeginDrag(std::shared_ptr<wm::Window> window, float start_x, float start_y)
{
    if (!window) {
        return false;
    }

    is_dragging_ = true;
    dragged_window_ = window;
    start_x_ = start_x;
    start_y_ = start_y;
    current_x_ = start_x;
    current_y_ = start_y;
    target_window_ = nullptr;
    current_quadrant_ = DropQuadrant::None;

    PRISM_LOG_INFO("TILING-DRAG", "Initiated titlebar drag for tile [%s] at (%.1f, %.1f)",
                   window->GetTitle().c_str(), start_x, start_y);
    return true;
}

void TilingDragManager::UpdateDrag(float current_x, float current_y,
                                   const std::vector<std::shared_ptr<wm::Window>> &all_windows)
{
    if (!is_dragging_ || !dragged_window_) {
        return;
    }

    current_x_ = current_x;
    current_y_ = current_y;

    // Minimum drag threshold to prevent accidental clicks
    float dist_sq = (current_x - start_x_) * (current_x - start_x_) +
                    (current_y - start_y_) * (current_y - start_y_);
    if (dist_sq < 25.0f) { // 5px threshold
        HideDropIndicator();
        return;
    }

    target_window_ = nullptr;
    current_quadrant_ = DropQuadrant::None;

    // Hit-test target tile
    for (const auto &win : all_windows) {
        if (!win) {
            continue;
        }
        auto b = win->GetBounds();
        if (current_x >= b.x && current_x <= b.x + b.width && current_y >= b.y &&
            current_y <= b.y + b.height) {
            target_window_ = win;

            float rx = current_x - b.x;
            float ry = current_y - b.y;
            float fx = rx / std::max(1.0f, b.width);
            float fy = ry / std::max(1.0f, b.height);

            // Quadrant detection: Left 25%, Right 25%, Top 25%, Bottom 25%, Center Swap
            if (fx < 0.25f) {
                current_quadrant_ = DropQuadrant::LeftSplit;
                preview_bounds_ = core::Rect{b.x, b.y, b.width * 0.5f, b.height};
            } else if (fx > 0.75f) {
                current_quadrant_ = DropQuadrant::RightSplit;
                preview_bounds_ = core::Rect{b.x + b.width * 0.5f, b.y, b.width * 0.5f, b.height};
            } else if (fy < 0.25f) {
                current_quadrant_ = DropQuadrant::TopSplit;
                preview_bounds_ = core::Rect{b.x, b.y, b.width, b.height * 0.5f};
            } else if (fy > 0.75f) {
                current_quadrant_ = DropQuadrant::BottomSplit;
                preview_bounds_ = core::Rect{b.x, b.y + b.height * 0.5f, b.width, b.height * 0.5f};
            } else {
                current_quadrant_ = DropQuadrant::Swap;
                preview_bounds_ = b;
            }
            break;
        }
    }

    if (target_window_ && current_quadrant_ != DropQuadrant::None) {
        UpdateDropIndicatorScene();
    } else {
        HideDropIndicator();
    }
}

DragDropResult TilingDragManager::EndDrag()
{
    DragDropResult result;
    if (!is_dragging_) {
        return result;
    }

    if (target_window_ && current_quadrant_ != DropQuadrant::None) {
        result.executed = true;
        result.source_window = dragged_window_;
        result.target_window = target_window_;
        result.quadrant = current_quadrant_;

        const char *quad_names[] = {"None",       "Swap",     "LeftSplit",
                                    "RightSplit", "TopSplit", "BottomSplit"};
        PRISM_LOG_INFO("TILING-DRAG", "Committed Drag-to-Split action '%s' from [%s] to [%s]",
                       quad_names[static_cast<int>(current_quadrant_)],
                       dragged_window_->GetTitle().c_str(), target_window_->GetTitle().c_str());
    }

    CancelDrag();
    return result;
}

void TilingDragManager::CancelDrag()
{
    is_dragging_ = false;
    dragged_window_ = nullptr;
    target_window_ = nullptr;
    current_quadrant_ = DropQuadrant::None;
    HideDropIndicator();
}

void TilingDragManager::UpdateDropIndicatorScene()
{
    if (!drop_zone_tree_) {
        return;
    }

    wlr_scene_node_set_enabled(&drop_zone_tree_->node, true);

    int px = static_cast<int>(preview_bounds_.x);
    int py = static_cast<int>(preview_bounds_.y);
    int pw = static_cast<int>(preview_bounds_.width);
    int ph = static_cast<int>(preview_bounds_.height);
    int bw = static_cast<int>(spec_->drop_zone.border_width);

    wlr_scene_node_set_position(&drop_zone_tree_->node, px, py);
    wlr_scene_rect_set_size(drop_zone_fill_, pw, ph);

    wlr_scene_rect_set_size(drop_border_top_, pw, bw);
    wlr_scene_node_set_position(&drop_border_top_->node, 0, 0);

    wlr_scene_rect_set_size(drop_border_bottom_, pw, bw);
    wlr_scene_node_set_position(&drop_border_bottom_->node, 0, std::max(0, ph - bw));

    wlr_scene_rect_set_size(drop_border_left_, bw, ph);
    wlr_scene_node_set_position(&drop_border_left_->node, 0, 0);

    wlr_scene_rect_set_size(drop_border_right_, bw, ph);
    wlr_scene_node_set_position(&drop_border_right_->node, std::max(0, pw - bw), 0);
}

void TilingDragManager::HideDropIndicator()
{
    if (drop_zone_tree_) {
        wlr_scene_node_set_enabled(&drop_zone_tree_->node, false);
    }
}

} // namespace prism::decoration
