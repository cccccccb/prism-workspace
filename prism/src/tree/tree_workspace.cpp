#include "prism/tree/tree_workspace.hpp"
#include <sstream>

namespace prism::tree {

WorkspaceNode::WorkspaceNode(int id, std::string name)
    : TreeNode(NodeType::Workspace), id_(id), name_(std::move(name)) {
    root_container_ = std::make_shared<ContainerNode>(LayoutMode::SplitHorizontal);
    AddChild(root_container_);
}

size_t WorkspaceNode::GetViewCount() {
    std::vector<std::shared_ptr<TreeNode>> views;
    CollectViews(views);
    return views.size();
}

void WorkspaceNode::Arrange(const core::Rect& screen_area, int inner_gap, int outer_gap, bool smart_gaps, float header_height) {
    bounds = screen_area;
    if (!root_container_ || !root_container_->HasChildren()) return;

    size_t count = GetViewCount();
    int eff_outer = outer_gap;
    int eff_inner = inner_gap;

    // Smart gaps: single window expands to full screen boundary
    if (smart_gaps && count == 1) {
        eff_outer = 0;
        eff_inner = 0;
    }

    core::Rect usable_area{
        screen_area.x + static_cast<float>(eff_outer),
        screen_area.y + static_cast<float>(eff_outer),
        std::max(0.0f, screen_area.width - 2.0f * static_cast<float>(eff_outer)),
        std::max(0.0f, screen_area.height - 2.0f * static_cast<float>(eff_outer))
    };

    root_container_->ArrangeChildren(usable_area, eff_inner, header_height);
}

std::string WorkspaceNode::ToJson(bool is_focused) const {
    std::ostringstream ss;
    ss << "{\"id\":" << id_
       << ",\"type\":\"workspace\""
       << ",\"name\":\"" << name_ << "\""
       << ",\"active\":" << (is_active_ ? "true" : "false")
       << ",\"focused\":" << (is_focused ? "true" : "false")
       << ",\"rect\":{\"x\":" << bounds.x << ",\"y\":" << bounds.y
       << ",\"width\":" << bounds.width << ",\"height\":" << bounds.height << "}"
       << ",\"nodes\":[";
    if (root_container_) {
        ss << root_container_->ToJson(false);
    }
    ss << "]}";
    return ss.str();
}

} // namespace prism::tree
