#pragma once
#include "prism/animation/group_geometry.hpp"
#include "prism/contracts/layout_snapshot.hpp"
#include "wlr_surface_geometry.hpp"
#include <span>

namespace prism::wm {
// Derives identity from tree traversal and inherited sides, never coordinate equality.
std::optional<animation::GroupGeometryLayout>
BuildGroupGeometry(const contracts::LayoutSnapshot &snapshot);

class GroupSurfaceGeometry {
public:
    GroupSurfaceGeometry();
    bool Start(const contracts::LayoutSnapshot &before, const contracts::LayoutSnapshot &after,
               std::span<WlrXdgView *const> views, const contracts::ThemeSnapshot &theme);
    void Restore();
    bool Prepare();
    bool SubmittedFrame(bool success);
    bool NeedsFrame() const;
    bool ValidTarget() const;
    SurfaceGeometry *ForView(WlrXdgView *view) const;
    SurfaceGeometry *ForNode(wlr_scene_node *node) const;
    SurfaceGeometry *ForSurface(wlr_surface *surface) const;

private:
    animation::SystemAnimationClock clock_;
    animation::GroupGeometryTimeline timeline_;

    struct Member {
        std::uint64_t id;
        std::unique_ptr<SurfaceGeometry> surface;
    };

    std::vector<Member> members_;
    std::uint64_t candidate_{};
};
} // namespace prism::wm
