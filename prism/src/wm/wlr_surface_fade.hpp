#pragma once
#include "prism/animation/timeline.hpp"
#include "prism/contracts/motion.hpp"

struct wlr_scene_tree;
struct wlr_scene_node;

namespace prism::wm {
struct WlrXdgView;
class SurfaceEffects;

// Compositor-owned opacity only. Live input geometry stays fixed. A closing
// snapshot owns buffer references, has no client identity and accepts no input.
class SurfaceFade {
public:
    SurfaceFade();
    ~SurfaceFade();
    SurfaceFade(const SurfaceFade &) = delete;
    SurfaceFade &operator=(const SurfaceFade &) = delete;
    void Open(WlrXdgView *view, SurfaceEffects *effects, contracts::MotionTransition spec);
    void Close(WlrXdgView *view, SurfaceEffects *effects);
    void Reset(SurfaceEffects *effects);
    bool Advance(SurfaceEffects *effects);
    bool Active() const noexcept;

private:
    void ClearSnapshot();
    void ApplyLive(SurfaceEffects *effects, float opacity);
    animation::DurationSpec Duration() const noexcept;
    animation::SystemAnimationClock clock_;
    animation::ScalarTimeline timeline_;
    contracts::MotionTransition spec_;
    WlrXdgView *live_{};
    wlr_scene_tree *snapshot_{};
    int x_{}, y_{};
    float opacity_{1};
    bool pending_{};
};
} // namespace prism::wm
