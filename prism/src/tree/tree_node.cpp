#include "prism/tree/tree_node.hpp"
#include "prism/ipc/wm_messages.hpp"
#include "prism/tree/tree_container.hpp"
#include "prism/tree/tree_workspace.hpp"
#include "prism/wm/window.hpp"
#include <atomic>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace prism::tree {

std::uint64_t AllocateTreeIdentity()
{
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    for (;;) {
        if (value == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Tree identity space exhausted");
        }
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) {
            return value;
        }
    }
}

TreeNode::TreeNode(NodeType t) : type(t), node_id_(AllocateTreeIdentity())
{
}

void TreeNode::MarkTopologyChanged()
{
    if (revisions_) {
        ++revisions_->topology;
        ++revisions_->layout;
    }
}

void TreeNode::MarkLayoutChanged()
{
    if (revisions_) {
        ++revisions_->layout;
    }
}

void TreeNode::MarkFocusChanged()
{
    if (revisions_) {
        ++revisions_->focus;
    }
}

void TreeNode::AttachRevisionState(const std::shared_ptr<TreeRevisionState> &state)
{
    const bool changed = revisions_ != state;
    if (changed && (type == NodeType::Container || type == NodeType::View)) {
        if (revisions_) {
            --revisions_->nodes;
        }
        if (state) {
            ++state->nodes;
        }
    }
    revisions_ = state;
    if (changed) {
        AttachmentChanged();
    }
    for (const auto &child : children_) {
        child->parent_ = weak_from_this();
        child->AttachRevisionState(state);
    }
}

void TreeNode::SetBounds(const core::Rect &bounds)
{
    if (bounds_ == bounds) {
        return;
    }
    if (!std::isfinite(bounds.x) || !std::isfinite(bounds.y) || !std::isfinite(bounds.width) ||
        !std::isfinite(bounds.height) || bounds.width < 0 || bounds.height < 0) {
        throw std::invalid_argument("Invalid tree bounds");
    }

    bounds_ = bounds;
    MarkLayoutChanged();
}

bool TreeNode::SetFractions(double width, double height)
{
    if (!std::isfinite(width) || !std::isfinite(height) || width < 0 || height < 0 ||
        width > 1000000 || height > 1000000) {
        return false;
    }
    if (width_fraction_ == width && height_fraction_ == height) {
        return true;
    }

    width_fraction_ = width;
    height_fraction_ = height;
    MarkLayoutChanged();
    return true;
}

void TreeNode::ChildrenChanged()
{
    MarkTopologyChanged();
}

void TreeNode::AttachmentChanged()
{
}

bool TreeNode::CanAdopt(const std::shared_ptr<TreeNode> &child) const
{
    if (type == NodeType::View || !child || child.get() == this) {
        return false;
    }
    auto ancestor = GetParent();
    while (ancestor) {
        if (ancestor == child) {
            return false;
        }
        ancestor = ancestor->GetParent();
    }
    return true;
}

std::shared_ptr<WorkspaceNode> TreeNode::GetWorkspace()
{
    auto cur = shared_from_this();
    while (cur) {
        if (cur->type == NodeType::Workspace) {
            return std::dynamic_pointer_cast<WorkspaceNode>(cur);
        }
        cur = cur->GetParent();
    }
    return nullptr;
}

std::shared_ptr<ContainerNode> TreeNode::GetParentContainer()
{
    auto p = GetParent();
    while (p) {
        if (p->type == NodeType::Container) {
            return std::dynamic_pointer_cast<ContainerNode>(p);
        }
        p = p->GetParent();
    }
    return nullptr;
}

std::shared_ptr<TreeNode> TreeNode::GetRoot()
{
    auto cur = shared_from_this();
    while (auto p = cur->GetParent()) {
        cur = p;
    }
    return cur;
}

void TreeNode::AddChild(std::shared_ptr<TreeNode> child, int index)
{
    if (!CanAdopt(child) || GetChildIndex(child) >= 0) {
        return;
    }
    if (auto previous = child->GetParent()) {
        previous->RemoveChild(child);
    }

    child->parent_ = weak_from_this();
    child->AttachRevisionState(revisions_);
    if (index < 0 || index >= static_cast<int>(children_.size())) {
        children_.push_back(std::move(child));
    } else {
        children_.insert(children_.begin() + index, std::move(child));
    }
    ChildrenChanged();
}

bool TreeNode::RemoveChild(const std::shared_ptr<TreeNode> &child)
{
    auto it = std::find(children_.begin(), children_.end(), child);
    if (it == children_.end()) {
        return false;
    }

    (*it)->parent_.reset();
    (*it)->AttachRevisionState(nullptr);
    children_.erase(it);
    ChildrenChanged();
    return true;
}

int TreeNode::GetChildIndex(const std::shared_ptr<TreeNode> &child) const
{
    for (size_t i = 0; i < children_.size(); ++i) {
        if (children_[i] == child) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void TreeNode::ReplaceChild(const std::shared_ptr<TreeNode> &old_child,
                            std::shared_ptr<TreeNode> new_child)
{
    const auto replaced = old_child;
    if (replaced == new_child || !CanAdopt(new_child) || GetChildIndex(replaced) < 0) {
        return;
    }
    if (auto previous = new_child->GetParent()) {
        previous->RemoveChild(new_child);
    }

    const auto idx = GetChildIndex(replaced);
    replaced->parent_.reset();
    replaced->AttachRevisionState(nullptr);
    new_child->parent_ = weak_from_this();
    new_child->AttachRevisionState(revisions_);
    children_[idx] = std::move(new_child);
    ChildrenChanged();
}

bool TreeNode::SwapWith(const std::shared_ptr<TreeNode> &other)
{
    const auto target = other;
    auto self = shared_from_this();
    auto first_parent = GetParent();
    auto second_parent = target ? target->GetParent() : nullptr;
    if (!target || target == self || !first_parent || !second_parent ||
        !first_parent->CanAdopt(target) || !second_parent->CanAdopt(self)) {
        return false;
    }

    const auto first_index = first_parent->GetChildIndex(self);
    const auto second_index = second_parent->GetChildIndex(target);
    if (first_index < 0 || second_index < 0) {
        return false;
    }

    first_parent->children_[first_index] = target;
    second_parent->children_[second_index] = self;
    parent_ = second_parent;
    target->parent_ = first_parent;
    AttachRevisionState(second_parent->revisions_);
    target->AttachRevisionState(first_parent->revisions_);

    const auto width = width_fraction_;
    const auto height = height_fraction_;
    SetFractions(target->width_fraction_, target->height_fraction_);
    target->SetFractions(width, height);
    first_parent->ChildrenChanged();
    if (second_parent != first_parent) {
        second_parent->ChildrenChanged();
    }
    return true;
}

void TreeNode::CollectViews(std::vector<std::shared_ptr<TreeNode>> &out_views)
{
    if (type == NodeType::View) {
        out_views.push_back(shared_from_this());
        return;
    }
    for (const auto &c : children_) {
        if (c) {
            c->CollectViews(out_views);
        }
    }
}

ipc::TreeNodeMessage TreeNode::ToMessage(bool is_focused) const
{
    ipc::TreeNodeMessage message;
    message.id = GetNodeId();
    message.type = "node";
    message.focused = is_focused;
    message.rect = ipc::RectMessage(GetBounds());
    message.fraction = ipc::FractionMessage{GetWidthFraction(), GetHeightFraction()};
    message.nodes.emplace();
    for (const auto &child : children_) {
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
    message.id = GetNodeId();
    message.type = "view";
    message.focused = is_focused;
    message.rect = ipc::RectMessage(GetBounds());
    message.fraction = ipc::FractionMessage{GetWidthFraction(), GetHeightFraction()};
    const auto target = window_ && window_->IsNative() ? window_->GetBounds() : GetBounds();
    message.name = window_ ? window_->GetTitle() : "";
    message.app_id = window_ ? window_->GetAppId() : "";
    message.focused = window_ && window_->IsFocused();
    message.native = window_ && window_->IsNative();
    message.pid = window_ ? window_->GetPid() : 0;
    message.instance = window_ ? window_->GetInstance() : 0;
    message.visible = window_ && window_->IsVisible();
    message.fullscreen = window_ && window_->IsFullscreen();
    message.rect = ipc::RectMessage(target);
    message.tile_rect = ipc::RectMessage(GetBounds());
    message.committed_rect =
        ipc::RectMessage(window_ ? window_->GetCommittedBounds() : core::Rect{});
    return message;
}

std::string ViewNode::ToJson(bool is_focused) const
{
    return nlohmann::json(ToMessage(is_focused)).dump();
}

} // namespace prism::tree
