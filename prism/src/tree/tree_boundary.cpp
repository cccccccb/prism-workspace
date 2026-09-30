#include "prism/tree/tree_container.hpp"

namespace prism::tree {

void ContainerNode::SetLayoutMode(LayoutMode mode)
{
    if (layout_mode_ == mode) {
        return;
    }

    layout_mode_ = mode;
    MarkTopologyChanged();
    RefreshBoundaries();
}

void ContainerNode::SetActiveChildIndex(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(GetChildren().size())) {
        return;
    }

    const auto selected = GetChildren()[idx];
    if (active_child_.lock() != selected) {
        MarkFocusChanged();
    }
    active_child_index_ = idx;
    active_child_ = selected;
}

void ContainerNode::ChildrenChanged()
{
    TreeNode::ChildrenChanged();

    const auto previous = active_child_.lock();
    const auto existing_index = GetChildIndex(previous);
    if (existing_index >= 0) {
        active_child_index_ = existing_index;
    } else if (!GetChildren().empty()) {
        SetActiveChildIndex(
            std::clamp(active_child_index_, 0, static_cast<int>(GetChildren().size() - 1)));
    } else {
        if (previous) {
            MarkFocusChanged();
        }
        active_child_.reset();
        active_child_index_ = 0;
    }

    RefreshBoundaries();
}

void ContainerNode::AttachmentChanged()
{
    // A detached subtree cannot revive an authority handle when it is reattached.
    boundaries_.clear();
    RefreshBoundaries();
}

void ContainerNode::RefreshBoundaries()
{
    if (layout_mode_ != LayoutMode::SplitHorizontal && layout_mode_ != LayoutMode::SplitVertical) {
        boundaries_.clear();
        return;
    }

    std::vector<BoundaryIdentity> current;
    const auto &children = GetChildren();
    for (std::size_t i = 1; i < children.size(); ++i) {
        const auto before = children[i - 1]->GetNodeId();
        const auto after = children[i]->GetNodeId();
        const auto found = std::find_if(boundaries_.begin(), boundaries_.end(),
                                        [before, after, this](const BoundaryIdentity &item) {
                                            return item.before == before && item.after == after &&
                                                   item.axis == layout_mode_;
                                        });
        const auto id = found != boundaries_.end() ? found->id : AllocateTreeIdentity();
        current.push_back({id, before, after, layout_mode_});
    }

    boundaries_ = std::move(current);
}

} // namespace prism::tree
