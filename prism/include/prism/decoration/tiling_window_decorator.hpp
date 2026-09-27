#pragma once

#include "prism/core/types.hpp"
#include "prism/decoration/motion_controller.hpp"
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
    TitlebarDrag, // User clicked on header blank space -> initiates Drag-to-Split / Reorder
    Close,        // User clicked Close tile button
    ToggleSplit,  // User clicked Toggle Split Orientation button
    ToggleFold,   // User clicked Fold (Roll-up / Unroll to titlebar) button
    ToggleMonocle // User clicked Monocle (Maximize/Restore) button
};

class TilingWindowDecorator {
public:
    TilingWindowDecorator(wm::Window *window, std::shared_ptr<TilingDecorationSpec> spec = nullptr);
    ~TilingWindowDecorator();

    // Attach scene graph nodes under a wlroots parent scene tree
    void AttachToScene(struct wlr_scene_tree *parent_tree);

    // Snap calculated tile bounds instantly
    void ApplyGeometry(const core::Rect &bounds);

    // Smooth Kinetic Transitions (Spring / Bézier from DSL)
    void AnimateToBounds(const core::Rect &bounds, MotionType type = MotionType::SplitMove);
    void ToggleFold();
    void SetFolded(bool folded);
    bool IsFolded() const;

    void ToggleFullscreen(const core::Rect &screen_bounds);
    void SetFullscreen(bool fullscreen, const core::Rect &screen_bounds);
    bool IsFullscreen() const;

    // Advances active kinetic animations (VSync driven). Returns true if still animating.
    bool StepAnimation(float dt);
    bool IsAnimating() const;

    // Focus state change (alters border colors, title brightness, specular glow)
    void SetFocused(bool focused);

    bool IsFocused() const
    {
        return is_focused_;
    }

    // Update styling spec dynamically (e.g. on theme swap)
    void SetSpec(std::shared_ptr<TilingDecorationSpec> spec);

    std::shared_ptr<TilingDecorationSpec> GetSpec() const
    {
        return spec_;
    }

    // Hit-testing within the window's local coordinates (relative to window top-left)
    HeaderAction HitTestHeader(float local_x, float local_y) const;

    // Computed sub-rectangles
    core::Rect GetHeaderBounds() const;
    core::Rect GetContentBounds() const;
    core::Rect GetVisualBounds() const;

    struct wlr_scene_tree *GetRootTree() const
    {
        return root_tree_;
    }

    struct wlr_scene_tree *GetContentTree() const
    {
        return content_tree_;
    }

    MotionController &GetMotionController()
    {
        return motion_ctrl_;
    }

private:
    void CreateSceneNodes();
    void UpdateColors();
    void UpdateSceneGeometry(const core::Rect &bounds);

    wm::Window *window_{nullptr};
    std::shared_ptr<TilingDecorationSpec> spec_;
    core::Rect current_bounds_{0, 0, 0, 0};
    bool is_focused_{false};

    // Kinetic Motion Physics Engine
    MotionController motion_ctrl_;

    // wlroots scene graph hierarchy
    struct wlr_scene_tree *parent_tree_{nullptr};
    struct wlr_scene_tree *root_tree_{nullptr};
    struct wlr_scene_rect *bg_rect_{nullptr};

    // Four borders for crisp tiling outline
    struct wlr_scene_rect *border_top_{nullptr};
    struct wlr_scene_rect *border_bottom_{nullptr};
    struct wlr_scene_rect *border_left_{nullptr};
    struct wlr_scene_rect *border_right_{nullptr};
    struct wlr_scene_rect *rim_specular_{nullptr}; // 1px specular top highlight

    // Header nodes
    struct wlr_scene_tree *header_tree_{nullptr};
    struct wlr_scene_rect *header_bg_{nullptr};
    struct wlr_scene_rect *btn_close_{nullptr};
    struct wlr_scene_rect *btn_split_{nullptr};
    struct wlr_scene_rect *btn_fold_{nullptr};
    struct wlr_scene_rect *btn_monocle_{nullptr};
    struct wlr_scene_rect *active_indicator_pill_{nullptr};

    // Child subtree where client contents (Wayland surface or AST views) are mounted
    struct wlr_scene_tree *content_tree_{nullptr};
};

} // namespace prism::decoration
