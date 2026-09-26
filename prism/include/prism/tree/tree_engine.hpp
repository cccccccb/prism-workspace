#pragma once

#include "prism/tree/tree_workspace.hpp"
#include "prism/decoration/tiling_decoration_spec.hpp"
#include <unordered_map>
#include <functional>

namespace prism::tree {

class TreeEngine {
public:
    TreeEngine();
    ~TreeEngine() = default;

    // Workspace Lifecycle
    std::shared_ptr<WorkspaceNode> GetOrCreateWorkspace(const std::string& name);
    bool SwitchWorkspace(const std::string& name);
    std::shared_ptr<WorkspaceNode> GetActiveWorkspace() const { return active_workspace_; }
    const std::vector<std::shared_ptr<WorkspaceNode>>& GetWorkspaces() const { return workspaces_; }

    // Window / View Insertion
    std::shared_ptr<ViewNode> InsertWindow(
        std::shared_ptr<wm::Window> win,
        Direction dir = Direction::Right,
        std::shared_ptr<TreeNode> target = nullptr
    );

    // Group into Tabbed / Stacked Container
    std::shared_ptr<ContainerNode> GroupTabbed(
        std::shared_ptr<TreeNode> target,
        std::shared_ptr<wm::Window> new_win
    );

    // Window Removal
    bool RemoveWindow(const std::shared_ptr<wm::Window>& win);

    // Tree Manipulation Operators
    bool SwapNodes(std::shared_ptr<TreeNode> a, std::shared_ptr<TreeNode> b);
    bool SwapFocusDirection(Direction dir);
    bool SetLayoutMode(std::shared_ptr<TreeNode> target, LayoutMode mode);
    bool SplitFocused(LayoutMode mode);
    bool MoveWindowToWorkspace(const std::shared_ptr<wm::Window>& win, const std::string& name);

    // Focus & Navigation
    void SetFocus(std::shared_ptr<TreeNode> node);
    void SetFocusedWindow(const std::shared_ptr<wm::Window>& win);
    std::shared_ptr<TreeNode> GetFocusedNode() const { return focused_node_.lock(); }
    std::shared_ptr<wm::Window> GetFocusedWindow() const;
    bool MoveFocus(Direction dir);

    // Layout Calculation
    void Arrange(const core::Rect& screen_area, const decoration::TilingDecorationSpec& spec);
    std::vector<std::pair<std::shared_ptr<wm::Window>, core::Rect>> GetCalculatedLayout() const;

    // Sway / i3 JSON Tree Introspection (swaymsg -t get_tree)
    std::string DumpTreeJson() const;

    // Search helpers
    std::shared_ptr<ViewNode> FindViewForWindow(const std::shared_ptr<wm::Window>& win) const;

private:
    std::vector<std::shared_ptr<WorkspaceNode>> workspaces_;
    std::shared_ptr<WorkspaceNode> active_workspace_;
    std::weak_ptr<TreeNode> focused_node_;

    int next_workspace_id_{1};
};

} // namespace prism::tree
