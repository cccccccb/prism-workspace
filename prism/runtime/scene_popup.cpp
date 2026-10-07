#include "scene_p.hpp"

#include <stdexcept>

namespace prism::runtime {
void Scene::ValidatePopupTree() const
{
    ValidateMenuTree();
    bool declarations = false;
    std::set<std::string> anchors;
    for (const auto &child : root_->children) {
        if (IsPopupKind(child->kind)) {
            declarations = true;
        } else if (declarations) {
            throw std::invalid_argument("Popup declarations must follow root content");
        }
    }
    for (const auto *node : nodes_) {
        if (!node) {
            continue;
        }
        if (!IsPopupKind(node->kind)) {
            if (!node->popup_for.empty()) {
                throw std::invalid_argument("popupFor belongs to Popup");
            }
            continue;
        }
        if (node->parent != root_.get() || node->popup_for.empty() ||
            !anchors.insert(node->popup_for).second || node->style.width <= 0 ||
            node->style.height <= 0 || !node->action.empty() || !node->region.empty()) {
            throw std::invalid_argument("Popup requires a unique anchor action and explicit size");
        }
        for (const auto &binding : node->bindings) {
            if (binding.target == DslProperty::PopupFor) {
                throw std::invalid_argument("Popup anchor identity must be literal");
            }
        }
        for (const auto &ref : node->theme_refs) {
            if (ref.target == DslProperty::PopupFor) {
                throw std::invalid_argument("Popup anchor identity cannot be themed");
            }
        }
        std::size_t matches = 0;
        for (const auto *anchor : nodes_) {
            if (!anchor || anchor->action != node->popup_for) {
                continue;
            }
            for (auto *parent = anchor; parent; parent = parent->parent) {
                if (parent->kind == Kind::Visual ||
                    (IsPopupKind(parent->kind) &&
                     (parent->kind != Kind::Menu || node->kind != Kind::Menu))) {
                    throw std::invalid_argument("Nested popup anchors are not supported yet");
                }
            }
            if (IsControlTarget(anchor->kind)) {
                throw std::invalid_argument("Popup trigger must be an action target");
            }
            ++matches;
        }
        if (matches != 1) {
            throw std::invalid_argument("Popup trigger action must resolve exactly once");
        }
    }
}

std::uint64_t Scene::PopupToken() const noexcept
{
    const auto *node = Find(active_popup_);
    return node && node->popup_token ? popup_epoch_ : 0;
}

bool Scene::InPopupScope(const Node &node) const
{
    const auto *popup = Find(active_popup_);
    return !popup || DescendantOf(&node, *popup);
}

bool Scene::OpenPopup(contracts::NodeId anchor_id, std::uint64_t seat)
{
    auto *anchor = Find(anchor_id);
    if (!anchor || !IsInteractive(anchor_id) || Has(dirty_, Dirty::Layout)) {
        return false;
    }
    Node *popup = nullptr;
    for (auto &child : root_->children) {
        if (IsPopupKind(child->kind) && child->popup_for == anchor->action &&
            child->style.visible && child->style.FitsViewport(viewport_) && IsEnabled(*child)) {
            popup = child.get();
        }
    }
    if (!popup) {
        return false;
    }
    auto *parent_popup = Find(active_popup_);
    const bool submenu = parent_popup && InPopupScope(*anchor) &&
                         parent_popup->kind == Kind::Menu && popup->kind == Kind::Menu;
    if (submenu &&
        std::any_of(popup_stack_.begin(), popup_stack_.end(),
                    [popup](const PopupFrame &frame) { return frame.node == popup->id; })) {
        return false;
    }
    const auto placement_anchor = submenu ? Find(popup_stack_.front().trigger) : anchor;
    if (!placement_anchor) {
        return false;
    }
    PopupPlacementRequest request{root_->bounds,
                                  placement_anchor->bounds,
                                  {popup->style.width, popup->style.height},
                                  std::min(160.0, popup->style.width),
                                  std::min(80.0, popup->style.height)};
    if (popup->contour_spec) {
        request.gap += contracts::PanelNeckHeight(*popup->contour_spec);
        request.horizontal_alignment = PopupHorizontalAlignment::Center;
    }
    if (!PlacePopup(request)) {
        return false;
    }
    if (!popup_session_) {
        popup_session_ = std::make_unique<PopupSession>(input_scene_id_);
    }

    popup_stack_.reserve(popup_stack_.size() + 1);
    const auto parent_token = submenu ? parent_popup->popup_token : 0;
    if (!submenu) {
        ClosePopup(PopupCloseReason::Replaced);
    }
    auto token = popup_session_->Open({input_scene_id_, input_scene_id_, anchor_id}, anchor_id,
                                      parent_token);
    if (!token) {
        return false;
    }
    DropPopupSurfaceAdoption();
    CancelInput();
    if (submenu) {
        parent_popup->popup_token = 0;
        ++parent_popup->revision;
    }
    popup_stack_.push_back({popup->id, anchor_id, *token});
    ++popup_epoch_;
    active_popup_ = popup->id;
    popup_seat_ = seat;
    popup->popup_token = *token;
    popup->popup_anchor = popup_stack_.front().trigger;
    ++popup->revision;
    ++transaction_revision_;
    Invalidate(Dirty::Layout | Dirty::Paint | Dirty::Composite);
    MoveInputFocus(seat, false);
    return true;
}

bool Scene::ClosePopup(PopupCloseReason reason)
{
    if (!active_popup_) {
        return false;
    }
    DropPopupSurfaceAdoption();
    auto *popup = Find(active_popup_);
    const auto anchor_id = popup ? popup->popup_anchor : contracts::NodeId{};
    if (popup) {
        CancelScrolledInput(*popup);
        popup->popup_token = 0;
        ++popup->revision;
    }
    if (popup_session_) {
        popup_session_->CloseAll(reason);
        popup_session_->TakeClosures();
    }
    popup_stack_.clear();
    ++popup_epoch_;
    active_popup_ = {};
    ++transaction_revision_;
    Invalidate(Dirty::Layout | Dirty::Paint | Dirty::Composite);

    if (IsInteractive(anchor_id)) {
        SetInputFocus(anchor_id, popup_seat_, true);
    } else {
        SetInputFocus({}, popup_seat_, false);
    }
    return true;
}

void Scene::ReconcilePopup()
{
    if (!active_popup_) {
        return;
    }
    for (const auto &frame : popup_stack_) {
        const auto *popup = Find(frame.node);
        const auto *anchor = Find(frame.trigger);
        if (!popup || !anchor || anchor->action != popup->popup_for) {
            ClosePopup(PopupCloseReason::Unavailable);
            return;
        }
        for (const auto *node : {popup, anchor}) {
            for (auto *parent = node; parent; parent = parent->parent) {
                // Suspended ancestor menus are hidden only by their frontend token.
                if (!parent->enabled || !parent->style.visible ||
                    !parent->style.FitsViewport(viewport_)) {
                    ClosePopup(PopupCloseReason::Unavailable);
                    return;
                }
            }
        }
    }
}

bool Scene::HandlePopupActivation(const Activation &activation, std::uint64_t seat)
{
    auto *node = Find(activation.node);
    if (!node) {
        return false;
    }
    if (node->kind == Kind::MenuBack) {
        BackPopup();
        return true;
    }
    for (const auto &child : root_->children) {
        if (IsPopupKind(child->kind) && child->popup_for == activation.action) {
            OpenPopup(node->id, seat);
            return true;
        }
    }
    if (active_popup_ && InPopupScope(*node) && !IsControlTarget(node->kind)) {
        ClosePopup(PopupCloseReason::Command);
    }
    return false;
}
} // namespace prism::runtime
