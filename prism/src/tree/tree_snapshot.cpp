#include "prism/tree/tree_engine.hpp"

namespace prism::tree {
namespace {

core::Rect BoundaryBounds(const core::Rect &parent, const core::Rect &before,
                          const core::Rect &after, LayoutMode axis)
{
    if (axis == LayoutMode::SplitHorizontal) {
        const auto start = before.x + before.width;
        return {start, parent.y, std::max(0.0f, after.x - start), parent.height};
    }

    const auto start = before.y + before.height;
    return {parent.x, start, parent.width, std::max(0.0f, after.y - start)};
}

void AppendNode(TreeSnapshot &snapshot, const std::shared_ptr<TreeNode> &node,
                std::uint64_t workspace)
{
    TreeNodeSnapshot item;
    item.id = node->GetNodeId();
    item.parent = node->GetParent() ? node->GetParent()->GetNodeId() : 0;
    item.workspace = workspace;
    item.type = node->type;
    item.bounds = node->GetBounds();
    item.width_fraction = node->GetWidthFraction();
    item.height_fraction = node->GetHeightFraction();
    for (const auto &child : node->GetChildren()) {
        item.children.push_back(child->GetNodeId());
    }

    if (const auto container = std::dynamic_pointer_cast<ContainerNode>(node)) {
        item.layout = container->GetLayoutMode();
        if (const auto active = container->GetActiveChild()) {
            item.active_child = active->GetNodeId();
        }
        const auto &children = container->GetChildren();
        const auto &boundaries = container->GetBoundaries();
        for (std::size_t i = 0; i < boundaries.size(); ++i) {
            const auto &boundary = boundaries[i];
            snapshot.boundaries.push_back(
                {boundary.id, item.id, workspace, boundary.before, boundary.after, boundary.axis,
                 BoundaryBounds(item.bounds, children[i]->GetBounds(), children[i + 1]->GetBounds(),
                                boundary.axis)});
        }
    }

    snapshot.nodes.push_back(std::move(item));
    for (const auto &child : node->GetChildren()) {
        AppendNode(snapshot, child, workspace);
    }
}

} // namespace

std::shared_ptr<const TreeSnapshot> TreeEngine::CaptureSnapshot() const
{
    if (snapshot_ && snapshot_->topology_revision == revisions_->topology &&
        snapshot_->layout_revision == revisions_->layout &&
        snapshot_->focus_revision == revisions_->focus) {
        return snapshot_;
    }

    auto snapshot = std::make_shared<TreeSnapshot>();
    snapshot->topology_revision = revisions_->topology;
    snapshot->layout_revision = revisions_->layout;
    snapshot->focus_revision = revisions_->focus;
    snapshot->active_workspace = active_workspace_ ? active_workspace_->GetNodeId() : 0;
    if (const auto focused = focused_node_.lock();
        focused && focused->GetWorkspace() == active_workspace_) {
        snapshot->focused_node = focused->GetNodeId();
    }
    for (const auto &workspace : workspaces_) {
        snapshot->workspaces.push_back({workspace->GetNodeId(),
                                        workspace->GetRootContainer()->GetNodeId(),
                                        workspace->GetName(), workspace->IsActive()});
        AppendNode(*snapshot, workspace, workspace->GetNodeId());
    }

    snapshot_ = std::move(snapshot);
    return snapshot_;
}

} // namespace prism::tree
