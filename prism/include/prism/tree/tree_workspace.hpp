#pragma once

#include "prism/tree/tree_container.hpp"
#include <string>

namespace prism::tree {

class WorkspaceNode : public TreeNode {
public:
    WorkspaceNode(int id, std::string name);

    int GetId() const
    {
        return id_;
    }

    const std::string &GetName() const
    {
        return name_;
    }

    bool IsActive() const
    {
        return is_active_;
    }

    std::shared_ptr<ContainerNode> GetRootContainer() const
    {
        return root_container_;
    }

    // Arrange the entire workspace subtree
    void Arrange(const core::Rect &screen_area, int inner_gap, int outer_gap, bool smart_gaps,
                 float header_height);

    // Count all views in this workspace
    size_t GetViewCount();

    ipc::TreeNodeMessage ToMessage(bool is_focused = false) const override;
    std::string ToJson(bool is_focused = false) const override;

private:
    friend class TreeEngine;

    void SetActive(bool active)
    {
        if (is_active_ == active) {
            return;
        }
        is_active_ = active;
        MarkLayoutChanged();
        MarkFocusChanged();
    }

    int id_{1};
    std::string name_{"1"};
    bool is_active_{false};
    std::shared_ptr<ContainerNode> root_container_;
};

} // namespace prism::tree
