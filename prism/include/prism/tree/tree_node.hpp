#pragma once

#include "prism/core/types.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace prism::wm {
class Window;
}

namespace prism::ipc {
struct TreeNodeMessage;
}

namespace prism::tree {

enum class NodeType { Root, Output, Workspace, Container, View };

enum class LayoutMode { None, SplitHorizontal, SplitVertical, Tabbed, Stacked };

enum class Direction { Left, Right, Up, Down };

class WorkspaceNode;
class ContainerNode;
class TreeEngine;

struct TreeRevisionState {
    std::uint64_t topology{1};
    std::uint64_t layout{1};
    std::uint64_t focus{1};
    std::size_t nodes{0};
};

std::uint64_t AllocateTreeIdentity();

/**
 * @brief Base Node for the Multi-Level Recursive Tree
 */
class TreeNode : public std::enable_shared_from_this<TreeNode> {
public:
    explicit TreeNode(NodeType t);
    virtual ~TreeNode() = default;
    TreeNode(const TreeNode &) = delete;
    TreeNode &operator=(const TreeNode &) = delete;

    const NodeType type;

    std::uint64_t GetNodeId() const noexcept
    {
        return node_id_;
    }

    const std::vector<std::shared_ptr<TreeNode>> &GetChildren() const noexcept
    {
        return children_;
    }

    const core::Rect &GetBounds() const noexcept
    {
        return bounds_;
    }

    double GetWidthFraction() const noexcept
    {
        return width_fraction_;
    }

    double GetHeightFraction() const noexcept
    {
        return height_fraction_;
    }

    void SetBounds(const core::Rect &bounds);
    bool SetFractions(double width, double height);

    std::shared_ptr<TreeNode> GetParent() const
    {
        return parent_.lock();
    }

    std::shared_ptr<WorkspaceNode> GetWorkspace();
    std::shared_ptr<ContainerNode> GetParentContainer();
    std::shared_ptr<TreeNode> GetRoot();

    // Child Management
    void AddChild(std::shared_ptr<TreeNode> child, int index = -1);
    bool RemoveChild(const std::shared_ptr<TreeNode> &child);
    int GetChildIndex(const std::shared_ptr<TreeNode> &child) const;
    void ReplaceChild(const std::shared_ptr<TreeNode> &old_child,
                      std::shared_ptr<TreeNode> new_child);

    bool SwapWith(const std::shared_ptr<TreeNode> &other);

    // Queries
    bool IsView() const
    {
        return type == NodeType::View;
    }

    bool IsContainer() const
    {
        return type == NodeType::Container;
    }

    bool IsWorkspace() const
    {
        return type == NodeType::Workspace;
    }

    bool HasChildren() const
    {
        return !children_.empty();
    }

    virtual std::shared_ptr<wm::Window> GetWindow() const
    {
        return nullptr;
    }

    void CollectViews(std::vector<std::shared_ptr<TreeNode>> &out_views);

    virtual ipc::TreeNodeMessage ToMessage(bool is_focused = false) const;
    virtual std::string ToJson(bool is_focused = false) const;

protected:
    virtual void ChildrenChanged();
    virtual void AttachmentChanged();
    void MarkTopologyChanged();
    void MarkLayoutChanged();
    void MarkFocusChanged();

private:
    friend class TreeEngine;
    std::weak_ptr<TreeNode> focused_inactive_child;
    void AttachRevisionState(const std::shared_ptr<TreeRevisionState> &state);
    bool CanAdopt(const std::shared_ptr<TreeNode> &child) const;

    const std::uint64_t node_id_;
    std::weak_ptr<TreeNode> parent_;
    std::vector<std::shared_ptr<TreeNode>> children_;
    core::Rect bounds_{};
    double width_fraction_{0};
    double height_fraction_{0};
    std::shared_ptr<TreeRevisionState> revisions_;
};

/**
 * @brief Leaf View Node holding a managed Window and its Decorator
 */
class ViewNode : public TreeNode {
public:
    explicit ViewNode(std::shared_ptr<wm::Window> win)
        : TreeNode(NodeType::View), window_(std::move(win))
    {
    }

    std::shared_ptr<wm::Window> GetWindow() const override
    {
        return window_;
    }

    ipc::TreeNodeMessage ToMessage(bool is_focused = false) const override;
    std::string ToJson(bool is_focused = false) const override;

private:
    std::shared_ptr<wm::Window> window_;
};

} // namespace prism::tree
