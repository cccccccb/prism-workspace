#include "prism/contracts/layout_snapshot.hpp"
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace prism::contracts {
namespace {
void Require(bool valid)
{
    if (!valid) {
        throw std::invalid_argument("Invalid WM layout snapshot");
    }
}

bool Text(std::string_view text)
{
    if (text.empty() || text.size() > 128) {
        return false;
    }
    for (std::size_t at = 0; at < text.size();) {
        const auto first = static_cast<unsigned char>(text[at++]);
        if (first < 0x20 || first == 0x7f) {
            return false;
        }
        if (first < 0x80) {
            continue;
        }
        unsigned remaining{};
        std::uint32_t code{}, minimum{};
        if ((first & 0xe0) == 0xc0) {
            remaining = 1;
            code = first & 31;
            minimum = 128;
        } else if ((first & 0xf0) == 0xe0) {
            remaining = 2;
            code = first & 15;
            minimum = 2048;
        } else if ((first & 0xf8) == 0xf0) {
            remaining = 3;
            code = first & 7;
            minimum = 65536;
        } else {
            return false;
        }
        if (remaining > text.size() - at) {
            return false;
        }
        while (remaining--) {
            const auto next = static_cast<unsigned char>(text[at++]);
            if ((next & 0xc0) != 0x80) {
                return false;
            }
            code = (code << 6) | (next & 63);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff) ||
            (code >= 0x80 && code <= 0x9f)) {
            return false;
        }
    }
    return true;
}

void Number(double value, double minimum, double maximum)
{
    Require(std::isfinite(value) && value >= minimum && value <= maximum);
}

void Rect(const LogicalRect &rect)
{
    Number(rect.x, -10000000, 10000000);
    Number(rect.y, -10000000, 10000000);
    Number(rect.width, 0, 10000000);
    Number(rect.height, 0, 10000000);
}

using Nodes = std::map<std::uint64_t, const LayoutNode *>;
using Workspaces = std::map<std::uint64_t, const LayoutWorkspace *>;

void ValidateGraph(const LayoutSnapshot &snapshot, const Nodes &nodes, const Workspaces &workspaces)
{
    std::vector<std::uint64_t> pending;
    std::set<std::uint64_t> visited;
    for (const auto &workspace : snapshot.workspaces) {
        const auto root = nodes.find(workspace.root);
        Require(root != nodes.end() && root->second->parent == 0 &&
                root->second->workspace == workspace.id &&
                root->second->kind == LayoutNodeKind::Container);
        pending.push_back(workspace.root);
    }

    for (std::size_t at = 0; at < pending.size(); ++at) {
        const auto &node = *nodes.at(pending[at]);
        Require(visited.insert(node.id).second);
        Require(workspaces.contains(node.workspace));
        for (const auto child_id : node.children) {
            const auto child = nodes.find(child_id);
            Require(child != nodes.end() && child->second->parent == node.id &&
                    child->second->workspace == node.workspace);
            pending.push_back(child_id);
            Require(pending.size() <= snapshot.nodes.size());
        }
    }
    Require(visited.size() == snapshot.nodes.size());
}

void ValidateBoundaries(const LayoutSnapshot &snapshot, const Nodes &nodes)
{
    using Edge = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;
    std::set<Edge> expected;
    for (const auto &node : snapshot.nodes) {
        if (node.layout == LayoutArrangement::Horizontal ||
            node.layout == LayoutArrangement::Vertical) {
            for (std::size_t i = 1; i < node.children.size(); ++i) {
                expected.emplace(node.id, node.children[i - 1], node.children[i]);
            }
        }
    }
    Require(expected.size() == snapshot.boundaries.size());

    std::set<std::uint64_t> identities;
    for (const auto &boundary : snapshot.boundaries) {
        Require(boundary.id && identities.insert(boundary.id).second && !boundary.resizable);
        Require(static_cast<unsigned>(boundary.axis) <= 1);
        Rect(boundary.bounds);
        const auto parent = nodes.find(boundary.parent);
        Require(parent != nodes.end() && parent->second->workspace == boundary.workspace);
        const auto layout = boundary.axis == LayoutBoundaryAxis::X ? LayoutArrangement::Horizontal
                                                                   : LayoutArrangement::Vertical;
        Require(parent->second->layout == layout);
        Require(expected.erase({boundary.parent, boundary.first, boundary.second}) == 1);
        Require(!boundary.visible ||
                (parent->second->visible && nodes.at(boundary.first)->visible &&
                 nodes.at(boundary.second)->visible));
    }
}
} // namespace

void ValidateLayoutSnapshot(const LayoutSnapshot &snapshot)
{
    Require(snapshot.session && snapshot.revision && snapshot.topology_revision &&
            snapshot.layout_revision && snapshot.focus_revision);
    Require(snapshot.outputs.size() <= kMaxLayoutOutputs &&
            snapshot.workspaces.size() <= kMaxLayoutWorkspaces &&
            snapshot.nodes.size() <= kMaxLayoutNodes &&
            snapshot.boundaries.size() <= kMaxLayoutBoundaries);

    std::set<std::uint64_t> outputs;
    unsigned primary_count{}, supported_count{};
    std::uint64_t primary_output{};
    for (const auto &output : snapshot.outputs) {
        Require(output.id && outputs.insert(output.id).second && Text(output.name));
        Rect(output.logical_bounds);
        Number(output.scale, 0.125, 16);
        primary_count += output.primary;
        if (output.primary) {
            primary_output = output.id;
        }
        supported_count += output.supported;
        Require(!output.supported || output.primary);
    }
    Require(primary_count <= 1 && supported_count <= 1);
    Require(snapshot.outputs.empty() || primary_count == 1);

    Workspaces workspaces;
    std::uint64_t active_workspace{};
    for (const auto &workspace : snapshot.workspaces) {
        Require(workspace.id && workspace.root && workspace.mode_revision &&
                workspaces.emplace(workspace.id, &workspace).second && Text(workspace.name));
        Require(workspace.mode == LayoutGroupMode::Normal ||
                workspace.mode == LayoutGroupMode::Immersive);
        Require(!workspace.output || outputs.contains(workspace.output));
        if (workspace.active) {
            Require(!active_workspace && workspace.output == primary_output);
            active_workspace = workspace.id;
        } else {
            Require(!workspace.output);
        }
    }
    Require(snapshot.workspaces.empty() || active_workspace);

    Nodes nodes;
    std::size_t child_count{};
    const LayoutNode *focused{};
    for (const auto &node : snapshot.nodes) {
        Require(node.id && nodes.emplace(node.id, &node).second);
        Require(static_cast<unsigned>(node.kind) <= 1 && static_cast<unsigned>(node.layout) <= 4);
        Require(node.children.size() <= kMaxLayoutNodes - child_count);
        child_count += node.children.size();
        Rect(node.tile_bounds);
        Rect(node.target_bounds);
        Rect(node.committed_bounds);
        Number(node.width_fraction, 0, 1000000);
        Number(node.height_fraction, 0, 1000000);
        if (node.kind == LayoutNodeKind::View) {
            Require(node.children.empty() && node.layout == LayoutArrangement::None);
        } else {
            Require(!node.instance.value && !node.fullscreen && !node.has_committed);
        }
        if (!node.has_committed) {
            Require(node.committed_bounds == LogicalRect{});
        }
        Require(!node.visible || (node.workspace == active_workspace && !snapshot.outputs.empty()));
        if (node.focused) {
            Require(!focused && node.kind == LayoutNodeKind::View &&
                    node.workspace == active_workspace && node.visible);
            focused = &node;
        }
    }
    Require(snapshot.active_instance == (focused ? focused->instance : InstanceId{}));

    ValidateGraph(snapshot, nodes, workspaces);
    ValidateBoundaries(snapshot, nodes);
}
} // namespace prism::contracts
