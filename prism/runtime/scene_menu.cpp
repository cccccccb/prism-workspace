#include "scene_p.hpp"

#include <stdexcept>

namespace prism::runtime {
void Scene::ValidateMenuTree() const
{
    for (const auto *node : nodes_) {
        if (!node) {
            continue;
        }
        if (IsMenuRow(node->kind)) {
            auto *owner = node->parent;
            while (owner && !IsPopupKind(owner->kind)) {
                owner = owner->parent;
            }
            if (!owner || owner->kind != Kind::Menu ||
                (node->kind == Kind::MenuItem && node->action.empty()) ||
                (node->kind == Kind::MenuBack && !node->action.empty())) {
                throw std::invalid_argument("Menu rows require a Menu and a valid command role");
            }
        }
        if (node->kind != Kind::Menu) {
            continue;
        }
        std::set<const Node *> path;
        const Node *current = node;
        while (current) {
            if (!path.insert(current).second) {
                throw std::invalid_argument("Menu anchor graph cannot contain cycles");
            }
            const Node *trigger = nullptr;
            for (const auto *candidate : nodes_) {
                if (candidate && candidate->action == current->popup_for) {
                    trigger = candidate;
                }
            }
            auto *owner = trigger ? trigger->parent : nullptr;
            while (owner && !IsPopupKind(owner->kind)) {
                owner = owner->parent;
            }
            if (owner) {
                bool back = false;
                for (const auto *candidate : nodes_) {
                    if (candidate && candidate->kind == Kind::MenuBack &&
                        DescendantOf(candidate, *current)) {
                        back = true;
                    }
                }
                if (!back) {
                    throw std::invalid_argument("Submenu needs an explicit MenuBack entry");
                }
            }
            current = owner;
        }
    }
}

bool Scene::BackPopup()
{
    if (popup_stack_.size() <= 1) {
        return ClosePopup(PopupCloseReason::Escape);
    }
    const auto frame = popup_stack_.back();
    auto *popup = Find(frame.node);
    auto *parent = Find(popup_stack_[popup_stack_.size() - 2].node);
    if (!popup || !parent) {
        return ClosePopup(PopupCloseReason::Unavailable);
    }

    DropPopupSurfaceAdoption();
    CancelInput();
    popup_session_->Escape();
    popup_session_->TakeClosures();
    popup->popup_token = 0;
    ++popup->revision;
    popup_stack_.pop_back();
    parent->popup_token = popup_stack_.back().token;
    ++parent->revision;
    active_popup_ = parent->id;
    ++popup_epoch_;
    ++transaction_revision_;
    Invalidate(Dirty::Layout | Dirty::Paint | Dirty::Composite);

    if (IsInteractive(frame.trigger)) {
        SetInputFocus(frame.trigger, popup_seat_, true);
    } else {
        MoveInputFocus(popup_seat_, false);
    }
    return true;
}

bool Scene::HandleMenuKey(const contracts::KeyEvent &event, const InputSnapshot *snapshot,
                          bool submitted)
{
    const auto *menu = Find(active_popup_);
    if (!menu || menu->kind != Kind::Menu) {
        return false;
    }
    const auto focus = std::find_if(
        input_state_->focus.begin(), input_state_->focus.end(),
        [&event](const InputState::Focus &item) { return item.seat == event.source.seat; });
    auto *node = focus == input_state_->focus.end() ? nullptr : Find(focus->node);
    if (!node || !IsMenuRow(node->kind) || !InPopupScope(*node)) {
        return false; // Direction keys belong to the focused value control.
    }
    const auto key = event.physical_key;
    if (key != 0x4f && key != 0x50 && key != 0x51 && key != 0x52 && key != 0x4a && key != 0x4d) {
        return false;
    }
    if (event.state == contracts::ButtonState::Released) {
        return true;
    }
    if (key == 0x50 || key == 0x4f) {
        if (!event.repeat) {
            if (key == 0x50 && popup_stack_.size() > 1) {
                BackPopup();
            } else if (key == 0x4f && node->kind == Kind::MenuItem) {
                OpenPopup(node->id, event.source.seat);
            }
        }
        return true;
    }

    std::vector<Node *> rows;
    for (auto *candidate : nodes_) {
        if (candidate && IsMenuRow(candidate->kind) && InPopupScope(*candidate) &&
            IsInteractive(candidate->id) &&
            (!submitted || IsInteractive(candidate->id, snapshot))) {
            rows.push_back(candidate);
        }
    }
    if (rows.empty()) {
        return true;
    }
    const auto current = std::find(rows.begin(), rows.end(), node);
    std::size_t index =
        current == rows.end() ? 0 : static_cast<std::size_t>(current - rows.begin());
    if (key == 0x4a) {
        index = 0;
    } else if (key == 0x4d) {
        index = rows.size() - 1;
    } else if (key == 0x52) {
        index = index ? index - 1 : rows.size() - 1;
    } else {
        index = (index + 1) % rows.size();
    }
    SetInputFocus(rows[index]->id, event.source.seat, true);
    return true;
}
} // namespace prism::runtime
