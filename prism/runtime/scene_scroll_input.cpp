#include "scene_p.hpp"

namespace prism::runtime {
void Scene::CancelScrolledInput(const Node &content) noexcept
{
    for (auto &pointer : input_state_->pointers) {
        if (DescendantOf(Find(pointer.captured), content)) {
            FinishGesture(pointer.gesture, contracts::GesturePhase::Cancel);
            pointer.captured = {};
            pointer.gesture = 0;
            pointer.action.clear();
        }
    }
    std::erase_if(input_state_->keys, [this, &content](const InputState::KeyPress &key) {
        return DescendantOf(Find(key.node), content);
    });
    for (const auto &touch : input_state_->touches) {
        if (DescendantOf(Find(touch.captured), content)) {
            FinishGesture(touch.gesture, contracts::GesturePhase::Cancel);
        }
    }
    std::erase_if(input_state_->touches, [this, &content](const InputState::Touch &touch) {
        return DescendantOf(Find(touch.captured), content);
    });
    for (std::size_t i = 0; i < input_state_->sliders.size(); ++i) {
        if (DescendantOf(Find(input_state_->sliders[i].node), content)) {
            FinishSlider(i, ValueCancelReason::Unavailable);
        }
    }
}

bool Scene::HandleScrollInput(const contracts::WindowEvent &event,
                              const std::shared_ptr<const InputSnapshot> &snapshot, bool submitted)
{
    const auto *wheel = std::get_if<contracts::PointerScrollEvent>(&event);
    if (!wheel || !std::isfinite(wheel->position.x) || !std::isfinite(wheel->position.y) ||
        !std::isfinite(wheel->delta_x) || !std::isfinite(wheel->delta_y) || wheel->delta_y == 0) {
        return false;
    }
    // Wheel bursts may refer to the last displayed position while a previous scroll is
    // awaiting submission. Only activation is gated on matching the new scroll offset.
    const auto hit = submitted ? (snapshot ? HitTest(wheel->position, *snapshot) : std::nullopt)
                               : HitTest(wheel->position);
    auto *node = hit ? Find(hit->node) : nullptr;
    if (!node || !IsInteractive(node->id)) {
        return false;
    }
    if (node->kind == Kind::TextField || node->kind == Kind::TextArea) {
        return false; // Existing editor scrolling retains its own input contract.
    }

    double remaining = wheel->delta_y;
    bool handled = false;
    for (; node; node = node->parent) {
        if (!InOwnerModalScope(*node)) {
            break;
        }
        if (node->kind != Kind::ScrollView || !IsEnabled(*node) || !IsVisible(*node)) {
            continue;
        }
        const auto *shown = snapshot ? snapshot->Find(node->id) : nullptr;
        if (submitted && (!shown || !shown->visible || !shown->enabled)) {
            continue;
        }
        handled = true;
        const auto local = submitted && IsPopupInputSnapshot(snapshot.get())
                               ? PopupScrollInfo(node->id)
                               : std::nullopt;
        const double before = local ? local->offset : node->scroll_offset;
        const double maximum =
            local ? local->maximum
                  : std::max(0.0, node->scroll_content_height - node->bounds.height);
        const double request = remaining > 0
                                   ? std::min(remaining, (maximum - before) / node->scroll_speed)
                                   : std::max(remaining, -before / node->scroll_speed);
        if (ScrollTo(node->id, before + request * node->scroll_speed)) {
            const auto after = local ? PopupScrollInfo(node->id) : std::nullopt;
            remaining -=
                ((after ? after->offset : node->scroll_offset) - before) / node->scroll_speed;
        }
        if (std::abs(remaining) < 1e-9) {
            break;
        }
    }
    return handled;
}

void Scene::RevealScrollTarget(contracts::NodeId id)
{
    if (RevealPopupScrollTarget(id)) {
        return;
    }
    auto *target = Find(id);
    if (!target || Has(dirty_, Dirty::Layout)) {
        return;
    }
    auto bounds = target->bounds;
    for (auto *parent = target->parent; parent; parent = parent->parent) {
        if (!InOwnerModalScope(*parent)) {
            break;
        }
        if (parent->kind != Kind::ScrollView) {
            continue;
        }
        double delta = 0;
        if (bounds.y < parent->bounds.y || bounds.height > parent->bounds.height) {
            delta = bounds.y - parent->bounds.y;
        } else if (bounds.y + bounds.height > parent->bounds.y + parent->bounds.height) {
            delta = bounds.y + bounds.height - parent->bounds.y - parent->bounds.height;
        }
        ScrollTo(parent->id, parent->scroll_offset + delta);
        // Outer viewports reveal the inner viewport, rather than trying to expose an
        // arbitrarily tall descendant that is already clipped by the inner viewport.
        bounds = parent->bounds;
    }
}
} // namespace prism::runtime
