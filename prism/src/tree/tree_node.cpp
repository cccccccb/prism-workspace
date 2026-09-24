#include "prism/tree/tree_node.hpp"
#include "prism/tree/tree_workspace.hpp"
#include "prism/tree/tree_container.hpp"
#include "prism/wm/window.hpp"
#include <sstream>

namespace prism::tree {

std::shared_ptr<WorkspaceNode> TreeNode::GetWorkspace() {
    auto cur = shared_from_this();
    while (cur) {
        if (cur->type == NodeType::Workspace) {
            return std::dynamic_pointer_cast<WorkspaceNode>(cur);
        }
        cur = cur->parent.lock();
    }
    return nullptr;
}

std::shared_ptr<ContainerNode> TreeNode::GetParentContainer() {
    auto p = parent.lock();
    while (p) {
        if (p->type == NodeType::Container) {
            return std::dynamic_pointer_cast<ContainerNode>(p);
        }
        p = p->parent.lock();
    }
    return nullptr;
}

std::shared_ptr<TreeNode> TreeNode::GetRoot() {
    auto cur = shared_from_this();
    while (auto p = cur->parent.lock()) {
        cur = p;
    }
    return cur;
}

void TreeNode::AddChild(std::shared_ptr<TreeNode> child, int index) {
    if (!child) return;
    child->parent = weak_from_this();
    if (index < 0 || index >= static_cast<int>(children.size())) {
        children.push_back(std::move(child));
    } else {
        children.insert(children.begin() + index, std::move(child));
    }
}

bool TreeNode::RemoveChild(const std::shared_ptr<TreeNode>& child) {
    if (!child) return false;
    auto it = std::find(children.begin(), children.end(), child);
    if (it != children.end()) {
        (*it)->parent.reset();
        children.erase(it);
        return true;
    }
    return false;
}

int TreeNode::GetChildIndex(const std::shared_ptr<TreeNode>& child) const {
    for (size_t i = 0; i < children.size(); ++i) {
        if (children[i] == child) return static_cast<int>(i);
    }
    return -1;
}

void TreeNode::ReplaceChild(const std::shared_ptr<TreeNode>& old_child, std::shared_ptr<TreeNode> new_child) {
    if (!old_child || !new_child) return;
    int idx = GetChildIndex(old_child);
    if (idx >= 0) {
        old_child->parent.reset();
        new_child->parent = weak_from_this();
        children[idx] = std::move(new_child);
    }
}

void TreeNode::CollectViews(std::vector<std::shared_ptr<TreeNode>>& out_views) {
    if (type == NodeType::View) {
        out_views.push_back(shared_from_this());
        return;
    }
    for (const auto& c : children) {
        if (c) c->CollectViews(out_views);
    }
}

std::string TreeNode::ToJson(bool is_focused) const {
    std::ostringstream ss;
    ss << "{\"id\":" << reinterpret_cast<uintptr_t>(this)
       << ",\"type\":\"node\""
       << ",\"focused\":" << (is_focused ? "true" : "false")
       << ",\"rect\":{\"x\":" << bounds.x << ",\"y\":" << bounds.y
       << ",\"width\":" << bounds.width << ",\"height\":" << bounds.height << "}"
       << ",\"fraction\":{\"width\":" << width_fraction << ",\"height\":" << height_fraction << "}"
       << ",\"nodes\":[";
    for (size_t i = 0; i < children.size(); ++i) {
        if (i > 0) ss << ",";
        ss << children[i]->ToJson(false);
    }
    ss << "]}";
    return ss.str();
}

std::string ViewNode::ToJson(bool is_focused) const {
    std::ostringstream ss;
    ss << "{\"id\":" << reinterpret_cast<uintptr_t>(this)
       << ",\"type\":\"view\""
       << ",\"name\":\"" << (window_ ? window_->GetTitle() : "") << "\""
       << ",\"app_id\":\"" << (window_ ? window_->GetAppId() : "") << "\""
       << ",\"focused\":" << (is_focused ? "true" : "false")
       << ",\"rect\":{\"x\":" << bounds.x << ",\"y\":" << bounds.y
       << ",\"width\":" << bounds.width << ",\"height\":" << bounds.height << "}"
       << ",\"fraction\":{\"width\":" << width_fraction << ",\"height\":" << height_fraction << "}"
       << "}";
    return ss.str();
}

} // namespace prism::tree
