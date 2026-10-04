#include "wlr_group_geometry.hpp"
#include "wlr_server_internal.hpp"

namespace prism::wm {
GroupSurfaceGeometry::GroupSurfaceGeometry() : timeline_(clock_)
{
}

bool GroupSurfaceGeometry::Start(const contracts::LayoutSnapshot &before,
                                 const contracts::LayoutSnapshot &after,
                                 std::span<WlrXdgView *const> views,
                                 const contracts::ThemeSnapshot &theme)
{
    const auto *spec = contracts::FindMotion(theme.motion, "group.geometry");
    auto from = BuildGroupGeometry(before), target = BuildGroupGeometry(after);
    if (!spec || !spec->duration_ms || !from || !target ||
        before.topology_revision != after.topology_revision) {
        return false;
    }
    if (from->edges == target->edges && from->members == target->members) {
        return !members_.empty();
    }
    if (members_.empty()) {
        // A new group cannot claim an uncommitted configure as its starting image.
        for (const auto &node : before.nodes) {
            if (node.kind == contracts::LayoutNodeKind::View && node.visible &&
                node.target_bounds != node.committed_bounds) {
                return false;
            }
        }
        if (!timeline_.Reset(*from)) {
            return false;
        }
    }
    const animation::DurationSpec duration{std::uint64_t(spec->duration_ms) * 1'000'000, 0,
                                           static_cast<animation::Easing>(spec->easing)};
    if (!timeline_.Retarget(*target, duration, clock_.NowNs())) {
        return false;
    }

    for (const auto &window : timeline_.Submitted().windows) {
        const auto node = std::find_if(after.nodes.begin(), after.nodes.end(),
                                       [&window](const auto &n) { return n.id == window.id; });
        if (node == after.nodes.end()) {
            return false;
        }
        const auto view = std::find_if(views.begin(), views.end(), [&node](const auto *v) {
            return v->instance == node->instance.value;
        });
        if (view == views.end()) {
            return false;
        }
        auto *surface = ForView(*view);
        if (!surface) {
            members_.push_back({window.id, std::make_unique<SurfaceGeometry>(*view)});
            surface = members_.back().surface.get();
        }
        const auto style = ResolveDecoration(&theme, node->focused, false, false).style;
        surface->Start(window.bounds, node->target_bounds, style, style,
                       {"group.geometry", 0, spec->easing});
    }
    return true;
}

void GroupSurfaceGeometry::Restore()
{
    for (auto &member : members_) {
        member.surface->Restore();
    }
}

bool GroupSurfaceGeometry::Prepare()
{
    if (!ValidTarget()) {
        return false;
    }
    const auto frame = timeline_.Prepare(clock_.NowNs());
    candidate_ = frame.serial;
    for (auto &member : members_) {
        const auto window = std::find_if(frame.windows.begin(), frame.windows.end(),
                                         [&member](const auto &w) { return w.id == member.id; });
        if (window == frame.windows.end() ||
            !member.surface->PrepareExternal({window->bounds, frame.generation, frame.state},
                                             member.surface->Decoration())) {
            return false;
        }
    }
    return true;
}

bool GroupSurfaceGeometry::SubmittedFrame(bool success)
{
    if (!success) {
        for (auto &member : members_) {
            member.surface->SubmittedFrame(false);
        }
        return false;
    }
    if (candidate_) {
        if (!timeline_.Accept(candidate_)) {
            return false;
        }
        candidate_ = 0;
    }
    bool settled = !timeline_.NeedsFrame();
    for (auto &member : members_) {
        settled = member.surface->SubmittedFrame(true) && settled;
    }
    return settled;
}

bool GroupSurfaceGeometry::NeedsFrame() const
{
    return timeline_.NeedsFrame() ||
           std::any_of(members_.begin(), members_.end(),
                       [](const auto &m) { return m.surface->NeedsFrame(); });
}

bool GroupSurfaceGeometry::ValidTarget() const
{
    return std::all_of(members_.begin(), members_.end(),
                       [](const auto &m) { return m.surface->ValidTarget(); });
}

SurfaceGeometry *GroupSurfaceGeometry::ForView(WlrXdgView *view) const
{
    for (const auto &member : members_) {
        if (member.surface->View() == view) {
            return member.surface.get();
        }
    }
    return nullptr;
}

SurfaceGeometry *GroupSurfaceGeometry::ForNode(wlr_scene_node *node) const
{
    for (const auto &member : members_) {
        if (&member.surface->View()->scene_tree->node == node) {
            return member.surface.get();
        }
    }
    return nullptr;
}

SurfaceGeometry *GroupSurfaceGeometry::ForSurface(wlr_surface *surface) const
{
    if (!surface) {
        return nullptr;
    }
    auto *root = wlr_surface_get_root_surface(surface);
    for (const auto &member : members_) {
        if (member.surface->View()->toplevel->base->surface == root) {
            return member.surface.get();
        }
    }
    return nullptr;
}
} // namespace prism::wm
