#include "prism/tree/tree_workspace.hpp"
#include "prism/ipc/wm_messages.hpp"
#include <cmath>
#include <nlohmann/json.hpp>

namespace prism::tree {

WorkspaceNode::WorkspaceNode(int id, std::string name)
    : TreeNode(NodeType::Workspace), id_(id), name_(std::move(name))
{
    root_container_ = std::make_shared<ContainerNode>(LayoutMode::SplitHorizontal);
    AddChild(root_container_);
}

size_t WorkspaceNode::GetViewCount()
{
    std::vector<std::shared_ptr<TreeNode>> views;
    CollectViews(views);
    return views.size();
}

void WorkspaceNode::Arrange(const core::Rect &screen_area, int inner_gap, int outer_gap,
                            bool smart_gaps, float header_height)
{
    SetBounds(screen_area);
    if (!root_container_) {
        return;
    }

    size_t count = GetViewCount();
    int eff_outer = std::max(0, outer_gap);
    int eff_inner = std::max(0, inner_gap);

    // Smart gaps: single window expands to full screen boundary
    if (smart_gaps && count == 1) {
        eff_outer = 0;
        eff_inner = 0;
    }
    // A valid theme can request gaps larger than a small output/work area.
    // Keep at least one logical pixel available before recursively splitting.
    const float shorter = std::max(0.0f, std::min(screen_area.width, screen_area.height));
    eff_outer =
        std::min(eff_outer, static_cast<int>(std::max(0.0f, std::floor((shorter - 1.0f) / 2.0f))));

    core::Rect usable_area{
        screen_area.x + static_cast<float>(eff_outer),
        screen_area.y + static_cast<float>(eff_outer),
        std::max(0.0f, screen_area.width - 2.0f * static_cast<float>(eff_outer)),
        std::max(0.0f, screen_area.height - 2.0f * static_cast<float>(eff_outer))};

    root_container_->ArrangeChildren(usable_area, eff_inner, header_height);
}

ipc::TreeNodeMessage WorkspaceNode::ToMessage(bool is_focused) const
{
    ipc::TreeNodeMessage message;
    message.id = GetNodeId();
    message.type = "workspace";
    message.name = name_;
    message.active = is_active_;
    message.focused = is_focused;
    message.rect = ipc::RectMessage(GetBounds());
    message.nodes.emplace();
    if (root_container_) {
        message.nodes->push_back(root_container_->ToMessage(false));
    }
    return message;
}

std::string WorkspaceNode::ToJson(bool is_focused) const
{
    return nlohmann::json(ToMessage(is_focused)).dump();
}

} // namespace prism::tree
