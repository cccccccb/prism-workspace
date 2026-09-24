#pragma once

#include "prism/core/types.hpp"
#include "prism/decoration/tiling_decoration_spec.hpp"
#include <memory>
#include <string>

// Forward declarations for wlroots
struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_scene_node;

namespace prism::wm {
class Window;
}

namespace prism::decoration {

enum class HeaderAction {
    None,
    TitlebarDrag,      // User clicked on header blank space -> initiates Drag-to-Split / Reorder
    Close,             // User clicked Close tile button
    ToggleSplit,       // User clicked Toggle Split Orientation button
    ToggleMonocle      // User clicked Monocle (Maximize/Restore) button
};

class TilingWindowDecorator {
public:
    TilingWindowDecorator(wm::Window* window, std::shared_ptr<TilingDecorationSpec> spec = nullptr);
    ~TilingWindowDecorator();

    // Attach scene graph nodes under a wlroots parent scene tree
    void AttachToScene(struct wlr_scene_tree* parent_tree);

    // Apply calculated tile bounds from Tiling Layout Engine
    void ApplyGeometry(const core::Rect& bounds);

    // Focus state change (alters border colors, title brightness, specular glow)
    void SetFocused(bool focused);
    bool IsFocused() const { return is_focused_; }

    // Update styling spec dynamically (e.g. on theme swap)
    void SetSpec(std::shared_ptr<TilingDecorationSpec> spec);
    std::shared_ptr<TilingDecorationSpec> GetSpec() const { return spec_; }

    // Hit-testing within the window's local coordinates (relative to window top-left)
    HeaderAction HitTestHeader(float local_x, float local_y) const;

    // Computed sub-rectangles
    core::Rect GetHeaderBounds() const;
    core::Rect GetContentBounds() const;

    struct wlr_scene_tree* GetRootTree() const { return root_tree_; }
    struct wlr_scene_tree* GetContentTree() const { return content_tree_; }

private:
    void CreateSceneNodes();
    void UpdateColors();

    wm::Window* window_{nullptr};
    std::shared_ptr<TilingDecorationSpec> spec_;
    core::Rect current_bounds_{0, 0, 0, 0};
    bool is_focused_{false};

    // wlroots scene graph hierarchy
    struct wlr_scene_tree* parent_tree_{nullptr};
    struct wlr_scene_tree* root_tree_{nullptr};
    struct wlr_scene_rect* bg_rect_{nullptr};

    // Four borders for crisp tiling outline
    struct wlr_scene_rect* border_top_{nullptr};
    struct wlr_scene_rect* border_bottom_{nullptr};
    struct wlr_scene_rect* border_left_{nullptr};
    struct wlr_scene_rect* border_right_{nullptr};
    struct wlr_scene_rect* rim_specular_{nullptr}; // 1px specular top highlight

    // Header nodes
    struct wlr_scene_tree* header_tree_{nullptr};
    struct wlr_scene_rect* header_bg_{nullptr};
    struct wlr_scene_rect* btn_close_{nullptr};
    struct wlr_scene_rect* btn_split_{nullptr};
    struct wlr_scene_rect* btn_monocle_{nullptr};
    struct wlr_scene_rect* active_indicator_pill_{nullptr};

    // Child subtree where client contents (Wayland surface or AST views) are mounted
    struct wlr_scene_tree* content_tree_{nullptr};
};

} // namespace prism::decoration
