#pragma once

#include "prism/core/types.hpp"
#include <memory>
#include <vector>
#include <string>
#include <algorithm>

namespace prism::wm {
class Window;
}

namespace prism::tree {

enum class NodeType {
    Root,
    Output,
    Workspace,
    Container,
    View
};

enum class LayoutMode {
    None,
    SplitHorizontal,
    SplitVertical,
    Tabbed,
    Stacked
};

enum class Direction {
    Left,
    Right,
    Up,
    Down
};

class WorkspaceNode;
class ContainerNode;

/**
 * @brief Base Node for the Multi-Level Recursive Tree
 */
class TreeNode : public std::enable_shared_from_this<TreeNode> {
public:
    explicit TreeNode(NodeType t) : type(t) {}
    virtual ~TreeNode() = default;

    NodeType type{NodeType::Container};
    std::weak_ptr<TreeNode> parent;
    std::vector<std::shared_ptr<TreeNode>> children;

    // Focused inactive child for precise focus restoration when switching back
    std::weak_ptr<TreeNode> focused_inactive_child;

    // Computed geometry
    core::Rect bounds{0, 0, 0, 0};

    // Normalized fraction in parent container (sum of sibling fractions == 1.0)
    double width_fraction{0.0};
    double height_fraction{0.0};

    // Tree Navigation
    std::shared_ptr<TreeNode> GetParent() const { return parent.lock(); }
    std::shared_ptr<WorkspaceNode> GetWorkspace();
    std::shared_ptr<ContainerNode> GetParentContainer();
    std::shared_ptr<TreeNode> GetRoot();

    // Child Management
    void AddChild(std::shared_ptr<TreeNode> child, int index = -1);
    bool RemoveChild(const std::shared_ptr<TreeNode>& child);
    int GetChildIndex(const std::shared_ptr<TreeNode>& child) const;
    void ReplaceChild(const std::shared_ptr<TreeNode>& old_child, std::shared_ptr<TreeNode> new_child);

    // Queries
    bool IsView() const { return type == NodeType::View; }
    bool IsContainer() const { return type == NodeType::Container; }
    bool IsWorkspace() const { return type == NodeType::Workspace; }
    bool HasChildren() const { return !children.empty(); }

    virtual std::shared_ptr<wm::Window> GetWindow() const { return nullptr; }
    void CollectViews(std::vector<std::shared_ptr<TreeNode>>& out_views);

    virtual std::string ToJson(bool is_focused = false) const;
};

/**
 * @brief Leaf View Node holding a managed Window and its Decorator
 */
class ViewNode : public TreeNode {
public:
    explicit ViewNode(std::shared_ptr<wm::Window> win)
        : TreeNode(NodeType::View), window_(std::move(win)) {}

    std::shared_ptr<wm::Window> GetWindow() const override { return window_; }

    std::string ToJson(bool is_focused = false) const override;

private:
    std::shared_ptr<wm::Window> window_;
};

} // namespace prism::tree
