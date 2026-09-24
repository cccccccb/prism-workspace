#include "prism/decoration/tiling_window_decorator.hpp"
#include "prism/wm/window.hpp"
#include "prism/core/logging.hpp"

extern "C" {
#include <wlr/types/wlr_scene.h>
}

namespace prism::decoration {

static inline void ColorToFloat4(const core::Color& c, float out[4]) {
    out[0] = static_cast<float>(c.r) / 255.0f;
    out[1] = static_cast<float>(c.g) / 255.0f;
    out[2] = static_cast<float>(c.b) / 255.0f;
    out[3] = static_cast<float>(c.a) / 255.0f;
}

TilingWindowDecorator::TilingWindowDecorator(wm::Window* window, std::shared_ptr<TilingDecorationSpec> spec)
    : window_(window), spec_(spec ? spec : TilingDecorationSpec::CreateDefault()) {
}

TilingWindowDecorator::~TilingWindowDecorator() {
    if (root_tree_) {
        wlr_scene_node_destroy(&root_tree_->node);
        root_tree_ = nullptr;
    }
}

void TilingWindowDecorator::AttachToScene(struct wlr_scene_tree* parent_tree) {
    if (!parent_tree) return;
    parent_tree_ = parent_tree;
    CreateSceneNodes();
}

void TilingWindowDecorator::CreateSceneNodes() {
    if (!parent_tree_) return;

    // Root tree for this window's decorator and content
    root_tree_ = wlr_scene_tree_create(parent_tree_);

    float bg_col[4];
    ColorToFloat4(is_focused_ ? spec_->backdrop.bg_focused : spec_->backdrop.bg_unfocused, bg_col);
    bg_rect_ = wlr_scene_rect_create(root_tree_, 100, 100, bg_col);

    // Border rectangles (4 edges for crisp tiling frame)
    float border_col[4];
    ColorToFloat4(is_focused_ ? spec_->border.color_focused : spec_->border.color_unfocused, border_col);
    border_top_ = wlr_scene_rect_create(root_tree_, 100, static_cast<int>(spec_->border.width), border_col);
    border_bottom_ = wlr_scene_rect_create(root_tree_, 100, static_cast<int>(spec_->border.width), border_col);
    border_left_ = wlr_scene_rect_create(root_tree_, static_cast<int>(spec_->border.width), 100, border_col);
    border_right_ = wlr_scene_rect_create(root_tree_, static_cast<int>(spec_->border.width), 100, border_col);

    // Top rim specular highlight (1px subtle luxury glow)
    float rim_col[4];
    ColorToFloat4(spec_->border.top_rim_specular, rim_col);
    rim_specular_ = wlr_scene_rect_create(root_tree_, 100, 1, rim_col);

    // Header tree
    header_tree_ = wlr_scene_tree_create(root_tree_);
    float header_bg_col[4];
    ColorToFloat4(is_focused_ ? spec_->header.bg_focused : spec_->header.bg_unfocused, header_bg_col);
    header_bg_ = wlr_scene_rect_create(header_tree_, 100, static_cast<int>(spec_->header.height), header_bg_col);

    // Tiling control buttons
    const float col_close[4]   = {0.98f, 0.28f, 0.28f, 1.0f}; // Red: Close
    const float col_split[4]   = {0.98f, 0.78f, 0.20f, 1.0f}; // Yellow: Toggle split H/V
    const float col_monocle[4] = {0.20f, 0.80f, 0.35f, 1.0f}; // Green: Monocle / Maximize
    btn_close_   = wlr_scene_rect_create(header_tree_, 12, 12, col_close);
    btn_split_   = wlr_scene_rect_create(header_tree_, 12, 12, col_split);
    btn_monocle_ = wlr_scene_rect_create(header_tree_, 12, 12, col_monocle);

    wlr_scene_node_set_position(&btn_close_->node, 14, 10);
    wlr_scene_node_set_position(&btn_split_->node, 34, 10);
    wlr_scene_node_set_position(&btn_monocle_->node, 54, 10);

    // Active status indicator pill (signals focused tile)
    float indicator_col[4];
    ColorToFloat4(spec_->border.color_focused, indicator_col);
    active_indicator_pill_ = wlr_scene_rect_create(header_tree_, 24, 4, indicator_col);
    wlr_scene_node_set_position(&active_indicator_pill_->node, 76, 14);
    wlr_scene_node_set_enabled(&active_indicator_pill_->node, is_focused_);

    // Client content subtree
    content_tree_ = wlr_scene_tree_create(root_tree_);

    if (current_bounds_.width > 0 && current_bounds_.height > 0) {
        ApplyGeometry(current_bounds_);
    }
}

void TilingWindowDecorator::ApplyGeometry(const core::Rect& bounds) {
    current_bounds_ = bounds;
    if (!root_tree_) return;

    int bx = static_cast<int>(bounds.x);
    int by = static_cast<int>(bounds.y);
    int bw = static_cast<int>(bounds.width);
    int bh = static_cast<int>(bounds.height);
    int border_w = static_cast<int>(spec_->border.width);
    int header_h = static_cast<int>(spec_->header.height);

    wlr_scene_node_set_position(&root_tree_->node, bx, by);

    if (bg_rect_) {
        wlr_scene_rect_set_size(bg_rect_, bw, bh);
    }

    if (border_top_) {
        wlr_scene_rect_set_size(border_top_, bw, border_w);
        wlr_scene_node_set_position(&border_top_->node, 0, 0);
    }
    if (border_bottom_) {
        wlr_scene_rect_set_size(border_bottom_, bw, border_w);
        wlr_scene_node_set_position(&border_bottom_->node, 0, std::max(0, bh - border_w));
    }
    if (border_left_) {
        wlr_scene_rect_set_size(border_left_, border_w, bh);
        wlr_scene_node_set_position(&border_left_->node, 0, 0);
    }
    if (border_right_) {
        wlr_scene_rect_set_size(border_right_, border_w, bh);
        wlr_scene_node_set_position(&border_right_->node, std::max(0, bw - border_w), 0);
    }

    if (rim_specular_) {
        wlr_scene_rect_set_size(rim_specular_, std::max(0, bw - 2 * border_w), 1);
        wlr_scene_node_set_position(&rim_specular_->node, border_w, border_w);
    }

    if (header_tree_ && header_bg_) {
        wlr_scene_node_set_enabled(&header_tree_->node, spec_->header.show_header);
        if (spec_->header.show_header) {
            wlr_scene_rect_set_size(header_bg_, std::max(0, bw - 2 * border_w), header_h);
            wlr_scene_node_set_position(&header_tree_->node, border_w, border_w);
        }
    }

    if (content_tree_) {
        int content_y = border_w + (spec_->header.show_header ? header_h : 0);
        wlr_scene_node_set_position(&content_tree_->node, border_w, content_y);
    }
}

void TilingWindowDecorator::SetFocused(bool focused) {
    if (is_focused_ == focused) return;
    is_focused_ = focused;
    UpdateColors();
}

void TilingWindowDecorator::SetSpec(std::shared_ptr<TilingDecorationSpec> spec) {
    if (!spec) return;
    spec_ = spec;
    UpdateColors();
    if (current_bounds_.width > 0 && current_bounds_.height > 0) {
        ApplyGeometry(current_bounds_);
    }
}

void TilingWindowDecorator::UpdateColors() {
    if (!root_tree_) return;

    float bg_col[4];
    ColorToFloat4(is_focused_ ? spec_->backdrop.bg_focused : spec_->backdrop.bg_unfocused, bg_col);
    if (bg_rect_) wlr_scene_rect_set_color(bg_rect_, bg_col);

    float border_col[4];
    ColorToFloat4(is_focused_ ? spec_->border.color_focused : spec_->border.color_unfocused, border_col);
    if (border_top_)    wlr_scene_rect_set_color(border_top_, border_col);
    if (border_bottom_) wlr_scene_rect_set_color(border_bottom_, border_col);
    if (border_left_)   wlr_scene_rect_set_color(border_left_, border_col);
    if (border_right_)  wlr_scene_rect_set_color(border_right_, border_col);

    float header_bg_col[4];
    ColorToFloat4(is_focused_ ? spec_->header.bg_focused : spec_->header.bg_unfocused, header_bg_col);
    if (header_bg_) wlr_scene_rect_set_color(header_bg_, header_bg_col);

    if (active_indicator_pill_) {
        wlr_scene_node_set_enabled(&active_indicator_pill_->node, is_focused_);
    }
}

HeaderAction TilingWindowDecorator::HitTestHeader(float local_x, float local_y) const {
    if (!spec_->header.show_header) return HeaderAction::None;

    float bw = spec_->border.width;
    float hh = spec_->header.height;

    // Check if within header bounds
    if (local_y < bw || local_y > bw + hh) return HeaderAction::None;
    if (local_x < bw || local_x > current_bounds_.width - bw) return HeaderAction::None;

    // Relative to header top-left
    float hx = local_x - bw;
    float hy = local_y - bw;

    // Close button: (14, 10, 12, 12) + 4px touch target padding
    if (hx >= 10.0f && hx <= 30.0f && hy >= 6.0f && hy <= 26.0f) {
        return HeaderAction::Close;
    }

    // Split button: (34, 10, 12, 12)
    if (hx >= 30.0f && hx <= 50.0f && hy >= 6.0f && hy <= 26.0f) {
        return HeaderAction::ToggleSplit;
    }

    // Monocle button: (54, 10, 12, 12)
    if (hx >= 50.0f && hx <= 70.0f && hy >= 6.0f && hy <= 26.0f) {
        return HeaderAction::ToggleMonocle;
    }

    // Anywhere else on the header: initiates Drag-to-Split / Tile Reorder!
    return HeaderAction::TitlebarDrag;
}

core::Rect TilingWindowDecorator::GetHeaderBounds() const {
    float bw = spec_->border.width;
    float hh = spec_->header.height;
    return core::Rect{
        current_bounds_.x + bw,
        current_bounds_.y + bw,
        std::max(0.0f, current_bounds_.width - 2 * bw),
        hh
    };
}

core::Rect TilingWindowDecorator::GetContentBounds() const {
    float bw = spec_->border.width;
    float hh = spec_->header.show_header ? spec_->header.height : 0.0f;
    return core::Rect{
        current_bounds_.x + bw,
        current_bounds_.y + bw + hh,
        std::max(0.0f, current_bounds_.width - 2 * bw),
        std::max(0.0f, current_bounds_.height - 2 * bw - hh)
    };
}

} // namespace prism::decoration
