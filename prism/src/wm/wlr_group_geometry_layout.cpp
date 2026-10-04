#include "wlr_group_geometry.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace prism::wm {
namespace {
using namespace contracts;
using namespace animation;
using Sides = std::array<std::uint64_t, 4>;

class Builder {
public:
    std::optional<GroupGeometryLayout> Build(const LayoutSnapshot &snapshot)
    {
        const LayoutWorkspace *active{};
        for (const auto &workspace : snapshot.workspaces) {
            if (workspace.active) {
                active = &workspace;
            }
        }
        if (!active || snapshot.outputs.size() != 1 || !snapshot.outputs.front().supported) {
            return {};
        }
        for (const auto &node : snapshot.nodes) {
            if (node.workspace == active->id) {
                nodes_.emplace(node.id, &node);
            }
        }
        const auto root = nodes_.find(active->root);
        if (root == nodes_.end()) {
            return {};
        }
        const auto &r = root->second->tile_bounds;
        const Sides sides{Edge(GeometryAxis::X, r.x), Edge(GeometryAxis::Y, r.y),
                          Edge(GeometryAxis::X, r.x + r.width),
                          Edge(GeometryAxis::Y, r.y + r.height)};
        if (!Visit(*root->second, sides, 0) || layout_.members.size() < 2) {
            return {};
        }
        return layout_;
    }

private:
    std::uint64_t Edge(GeometryAxis axis, double position)
    {
        const auto id = layout_.edges.size() + 1;
        layout_.edges.push_back({id, axis, std::round(position)});
        return id;
    }

    GroupEdgeRef Ref(std::uint64_t edge, double position) const
    {
        return {edge, int(std::round(position) - layout_.edges[edge - 1].position)};
    }

    bool Visit(const LayoutNode &node, Sides sides, unsigned depth)
    {
        if (depth > 64 || !node.visible || node.fullscreen) {
            return false;
        }
        if (node.kind == LayoutNodeKind::View) {
            if (!node.has_committed) {
                return false;
            }
            const auto &r = node.target_bounds;
            layout_.members.push_back({node.id, Ref(sides[0], r.x), Ref(sides[1], r.y),
                                       Ref(sides[2], r.x + r.width),
                                       Ref(sides[3], r.y + r.height)});
            return true;
        }
        if (node.children.empty() || (node.layout != LayoutArrangement::Horizontal &&
                                      node.layout != LayoutArrangement::Vertical)) {
            return false;
        }
        const bool horizontal = node.layout == LayoutArrangement::Horizontal;
        const int leading = horizontal ? 0 : 1, trailing = horizontal ? 2 : 3;
        auto start = sides[leading];
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            const auto found = nodes_.find(node.children[i]);
            if (found == nodes_.end()) {
                return false;
            }
            const auto &child = *found->second;
            auto end = sides[trailing];
            if (i + 1 < node.children.size()) {
                const auto &r = child.tile_bounds;
                end = Edge(horizontal ? GeometryAxis::X : GeometryAxis::Y,
                           horizontal ? r.x + r.width : r.y + r.height);
            }
            auto child_sides = sides;
            child_sides[leading] = start;
            child_sides[trailing] = end;
            if (!Visit(child, child_sides, depth + 1)) {
                return false;
            }
            start = end;
        }
        return true;
    }

    std::map<std::uint64_t, const LayoutNode *> nodes_;
    GroupGeometryLayout layout_;
};
} // namespace

std::optional<animation::GroupGeometryLayout>
BuildGroupGeometry(const contracts::LayoutSnapshot &snapshot)
{
    return Builder().Build(snapshot);
}
} // namespace prism::wm
