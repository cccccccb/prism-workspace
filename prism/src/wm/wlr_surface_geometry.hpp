#pragma once
#include "prism/animation/geometry.hpp"
#include "prism/contracts/theme.hpp"
#include <memory>
#include <vector>

struct wlr_surface;
struct wlr_scene_node;

namespace prism::wm {
struct WlrXdgView;

class SurfaceGeometry {
public:
    explicit SurfaceGeometry(WlrXdgView *view);
    ~SurfaceGeometry();
    SurfaceGeometry(const SurfaceGeometry &) = delete;
    SurfaceGeometry &operator=(const SurfaceGeometry &) = delete;
    WlrXdgView *View() const noexcept;
    contracts::LogicalRect Submitted() const noexcept;
    contracts::ThemeDecoration Decoration() const noexcept;
    void Start(contracts::LogicalRect from, contracts::LogicalRect target,
               contracts::ThemeDecoration from_style, contracts::ThemeDecoration target_style,
               const contracts::MotionTransition &spec);
    void Restore();
    bool Prepare();
    bool SubmittedFrame(bool success);
    bool NeedsFrame() const noexcept;
    bool ValidTarget() const noexcept;
    wlr_surface *Hit(double x, double y, double &sx, double &sy) const;
    bool Position(wlr_surface *surface, double x, double y, double &sx, double &sy) const;

private:
    struct SavedNode;
    bool Capture(wlr_scene_node *node);
    void Transform();
    animation::SystemAnimationClock clock_;
    animation::GeometryTimeline geometry_;
    animation::ScalarTimeline decoration_;
    WlrXdgView *view_;
    animation::GeometrySample candidate_, submitted_;
    contracts::LogicalRect source_, submitted_source_;
    contracts::ThemeDecoration from_style_, target_style_, submitted_style_;
    std::vector<std::unique_ptr<SavedNode>> saved_;
    bool retry_{}, dirty_{true};
};
} // namespace prism::wm
