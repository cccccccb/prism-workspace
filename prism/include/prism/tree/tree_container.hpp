#pragma once

#include "prism/tree/tree_node.hpp"
#include <string>

namespace prism::tree {

class ContainerNode : public TreeNode {
public:
    explicit ContainerNode(LayoutMode mode = LayoutMode::SplitHorizontal)
        : TreeNode(NodeType::Container), layout_mode_(mode)
    {
    }

    LayoutMode GetLayoutMode() const
    {
        return layout_mode_;
    }

    void SetLayoutMode(LayoutMode mode)
    {
        layout_mode_ = mode;
    }

    int GetActiveChildIndex() const
    {
        return active_child_index_;
    }

    void SetActiveChildIndex(int idx)
    {
        if (idx >= 0 && idx < static_cast<int>(children.size())) {
            active_child_index_ = idx;
        }
    }

    std::shared_ptr<TreeNode> GetActiveChild() const
    {
        if (children.empty()) {
            return nullptr;
        }
        int idx = std::clamp(active_child_index_, 0, static_cast<int>(children.size() - 1));
        return children[idx];
    }

    // Fraction Normalization
    void NormalizeFractions();

    // Recursive Layout Placement
    void ArrangeChildren(const core::Rect &area, int inner_gap, float header_height);

    // Tree Simplification (prunes empty or redundant single-child wrapper containers)
    bool AutoPrune();

    ipc::TreeNodeMessage ToMessage(bool is_focused = false) const override;
    std::string ToJson(bool is_focused = false) const override;

private:
    void ArrangeSplitHorizontal(const core::Rect &area, int inner_gap, float header_height);
    void ArrangeSplitVertical(const core::Rect &area, int inner_gap, float header_height);
    void ArrangeTabbed(const core::Rect &area, float header_height);
    void ArrangeStacked(const core::Rect &area, float header_height);

    LayoutMode layout_mode_{LayoutMode::SplitHorizontal};
    int active_child_index_{0};
};

} // namespace prism::tree
