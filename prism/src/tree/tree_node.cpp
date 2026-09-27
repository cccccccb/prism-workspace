#include "prism/tree/tree_node.hpp"
#include "prism/ipc/wm_messages.hpp"
#include "prism/tree/tree_container.hpp"
#include "prism/tree/tree_workspace.hpp"
#include "prism/wm/window.hpp"
#include <nlohmann/json.hpp>

namespace prism::tree {

std::shared_ptr<WorkspaceNode> TreeNode::GetWorkspace()
{
    auto cur = shared_from_this();
    while (cur) {
        if (cur->type == NodeType::Workspace) {
            return std::dynamic_pointer_cast<WorkspaceNode>(cur);
        }
        cur = cur->parent.lock();
    }
    return nullptr;
}

std::shared_ptr<ContainerNode> TreeNode::GetParentContainer()
{
    auto p = parent.lock();
    while (p) {
        if (p->type == NodeType::Container) {
            return std::dynamic_pointer_cast<ContainerNode>(p);
        }
        p = p->parent.lock();
    }
    return nullptr;
}

std::shared_ptr<TreeNode> TreeNode::GetRoot()
{
    auto cur = shared_from_this();
    while (auto p = cur->parent.lock()) {
        cur = p;
    }
    return cur;
}

void TreeNode::AddChild(std::shared_ptr<TreeNode> child, int index)
{
    if (!child) {
        return;
    }
    child->parent = weak_from_this();
    if (index < 0 || index >= static_cast<int>(children.size())) {
        children.push_back(std::move(child));
    } else {
        children.insert(children.begin() + index, std::move(child));
    }
}

bool TreeNode::RemoveChild(const std::shared_ptr<TreeNode> &child)
{
    if (!child) {
        return false;
    }
    auto it = std::find(children.begin(), children.end(), child);
    if (it != children.end()) {
        (*it)->parent.reset();
        children.erase(it);
        return true;
    }
    return false;
}

int TreeNode::GetChildIndex(const std::shared_ptr<TreeNode> &child) const
{
    for (size_t i = 0; i < children.size(); ++i) {
        if (children[i] == child) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void TreeNode::ReplaceChild(const std::shared_ptr<TreeNode> &old_child,
                            std::shared_ptr<TreeNode> new_child)
{
    if (!old_child || !new_child) {
        return;
    }
    int idx = GetChildIndex(old_child);
    if (idx >= 0) {
        old_child->parent.reset();
        new_child->parent = weak_from_this();
        children[idx] = std::move(new_child);
    }
}

void TreeNode::CollectViews(std::vector<std::shared_ptr<TreeNode>> &out_views)
{
    if (type == NodeType::View) {
        out_views.push_back(shared_from_this());
        return;
    }
    for (const auto &c : children) {
        if (c) {
            c->CollectViews(out_views);
        }
    }
}

ipc::TreeNodeMessage TreeNode::ToMessage(bool is_focused) const
{
    ipc::TreeNodeMessage message;
    message.id = reinterpret_cast<std::uintptr_t>(this);
    message.type = "node";
    message.focused = is_focused;
    message.rect = ipc::RectMessage(bounds);
    message.fraction = ipc::FractionMessage{width_fraction, height_fraction};
    message.nodes.emplace();
    for (const auto &child : children) {
        message.nodes->push_back(child->ToMessage(false));
    }
    return message;
}

std::string TreeNode::ToJson(bool is_focused) const
{
    return nlohmann::json(ToMessage(is_focused)).dump();
}

ipc::TreeNodeMessage ViewNode::ToMessage(bool is_focused) const
{
    ipc::TreeNodeMessage message;
    message.id = reinterpret_cast<std::uintptr_t>(this);
    message.type = "view";
    message.focused = is_focused;
    message.rect = ipc::RectMessage(bounds);
    message.fraction = ipc::FractionMessage{width_fraction, height_fraction};
    const auto target = window_ && window_->IsNative() ? window_->GetBounds() : bounds;
    message.name = window_ ? window_->GetTitle() : "";
    message.app_id = window_ ? window_->GetAppId() : "";
    message.focused = window_ && window_->IsFocused();
    message.native = window_ && window_->IsNative();
    message.pid = window_ ? window_->GetPid() : 0;
    message.instance = window_ ? window_->GetInstance() : 0;
    message.visible = window_ && window_->IsVisible();
    message.fullscreen = window_ && window_->IsFullscreen();
    message.rect = ipc::RectMessage(target);
    message.tile_rect = ipc::RectMessage(bounds);
    message.committed_rect =
        ipc::RectMessage(window_ ? window_->GetCommittedBounds() : core::Rect{});
    return message;
}

std::string ViewNode::ToJson(bool is_focused) const
{
    return nlohmann::json(ToMessage(is_focused)).dump();
}

} // namespace prism::tree
