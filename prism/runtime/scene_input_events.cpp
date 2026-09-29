#include "scene_p.hpp"

namespace prism::runtime {
namespace {
constexpr std::uint32_t EnterKey = 0x28;
constexpr std::uint32_t EscapeKey = 0x29;
constexpr std::uint32_t TabKey = 0x2b;
constexpr std::uint32_t SpaceKey = 0x2c;
} // namespace

void Scene::MoveInputPointer(contracts::InputSource source, contracts::LogicalPoint point)
{
    auto &pointers = input_state_->pointers;
    auto current = std::find_if(
        pointers.begin(), pointers.end(),
        [source](const InputState::Pointer &pointer) { return pointer.source == source; });
    const bool inside = scene_detail::Inside({0, 0, viewport_.width, viewport_.height}, point);
    const auto hit = inside ? HitTest(point) : std::nullopt;
    const auto id = hit ? hit->node : contracts::NodeId{};
    TrackInputTarget(id);
    if (current == pointers.end()) {
        if (inside) {
            pointers.push_back({source, id, {}, {}, point, true});
        }
    } else {
        current->hovered = id;
        current->position = point;
        current->inside = inside;
    }
}

void Scene::LeaveInputPointer(contracts::InputSource source, bool cancel)
{
    for (auto &pointer : input_state_->pointers) {
        if (pointer.source != source) {
            continue;
        }
        pointer.hovered = {};
        pointer.inside = false;
        if (cancel) {
            pointer.captured = {};
            pointer.action.clear();
        }
    }
}

std::optional<Activation> Scene::HandleInputButton(const contracts::PointerButtonEvent &event)
{
    auto &pointers = input_state_->pointers;
    auto current = std::find_if(
        pointers.begin(), pointers.end(),
        [&event](const InputState::Pointer &pointer) { return pointer.source == event.source; });
    if (current == pointers.end() && event.state == contracts::ButtonState::Released) {
        return std::nullopt;
    }

    // Wayland buttons carry the adapter's last position. After leave that
    // position is stale; only an enter or motion can restore a captured stream.
    if (current == pointers.end() || current->inside) {
        MoveInputPointer(event.source, event.position);
    }
    if (event.button != contracts::PointerButton::Primary) {
        return std::nullopt;
    }
    current = std::find_if(
        pointers.begin(), pointers.end(),
        [&event](const InputState::Pointer &pointer) { return pointer.source == event.source; });
    if (current == pointers.end()) {
        return std::nullopt;
    }

    if (event.state == contracts::ButtonState::Pressed) {
        if (current->captured || !IsInteractive(current->hovered)) {
            return std::nullopt;
        }
        current->captured = current->hovered;
        current->action = Find(current->captured)->action;
        SetInputFocus(current->captured, event.source.seat, false);
        return std::nullopt;
    }

    std::optional<Activation> activation;
    const auto *node = Find(current->captured);
    if (IsInteractive(current->captured) && current->hovered == current->captured &&
        node->action == current->action) {
        activation = Activation{node->id, current->action};
    }
    current->captured = {};
    current->action.clear();
    return activation;
}

std::optional<Activation> Scene::HandleInputKey(const contracts::KeyEvent &event)
{
    if (event.repeat) {
        return std::nullopt;
    }
    const bool down = event.state == contracts::ButtonState::Pressed;
    if (down && event.physical_key == EscapeKey) {
        for (auto &pointer : input_state_->pointers) {
            if (pointer.source.seat == event.source.seat) {
                pointer.captured = {};
                pointer.action.clear();
            }
        }
        std::erase_if(input_state_->keys, [&event](const InputState::KeyPress &key) {
            return key.source.seat == event.source.seat;
        });
        return std::nullopt;
    }
    if (event.modifiers.control || event.modifiers.alt || event.modifiers.meta) {
        std::erase_if(input_state_->keys, [&event](const InputState::KeyPress &key) {
            return key.source == event.source;
        });
        return std::nullopt;
    }
    if (down && event.physical_key == TabKey) {
        MoveInputFocus(event.source.seat, event.modifiers.shift);
        return std::nullopt;
    }
    if (event.physical_key != EnterKey && event.physical_key != SpaceKey) {
        return std::nullopt;
    }

    const auto focus = std::find_if(
        input_state_->focus.begin(), input_state_->focus.end(),
        [&event](const InputState::Focus &item) { return item.seat == event.source.seat; });
    auto &keys = input_state_->keys;
    const auto current =
        std::find_if(keys.begin(), keys.end(), [&event](const InputState::KeyPress &key) {
            return key.source == event.source;
        });
    if (down) {
        if (current != keys.end() || focus == input_state_->focus.end() ||
            !IsInteractive(focus->node)) {
            return std::nullopt;
        }
        focus->visible = true;
        keys.push_back({event.source, focus->node, event.physical_key, Find(focus->node)->action});
        return std::nullopt;
    }
    if (current == keys.end() || current->key != event.physical_key) {
        return std::nullopt;
    }

    std::optional<Activation> activation;
    const auto *node = Find(current->node);
    if (focus != input_state_->focus.end() && focus->node == current->node &&
        IsInteractive(current->node) && node->action == current->action) {
        activation = Activation{node->id, current->action};
    }
    keys.erase(current);
    return activation;
}

InteractionResult Scene::HandleInput(const contracts::WindowEvent &event)
{
    InteractionResult result;
    if (const auto *motion = std::get_if<contracts::PointerMotionEvent>(&event)) {
        MoveInputPointer(motion->source, motion->position);
    } else if (const auto *enter = std::get_if<contracts::PointerEnterEvent>(&event)) {
        MoveInputPointer(enter->source, enter->position);
    } else if (const auto *leave = std::get_if<contracts::PointerLeaveEvent>(&event)) {
        LeaveInputPointer(leave->source, false);
    } else if (const auto *cancel = std::get_if<contracts::PointerCancelEvent>(&event)) {
        LeaveInputPointer(cancel->source, true);
    } else if (const auto *button = std::get_if<contracts::PointerButtonEvent>(&event)) {
        result.activation = HandleInputButton(*button);
    } else if (const auto *key = std::get_if<contracts::KeyEvent>(&event)) {
        result.activation = HandleInputKey(*key);
    } else if (const auto *focus = std::get_if<contracts::FocusEvent>(&event)) {
        if (!focus->focused) {
            CancelSeatInput(focus->source.seat);
        }
    } else if (std::holds_alternative<contracts::CloseRequestedEvent>(event)) {
        result.changed = CancelInput();
    }

    result.changed = ReconcileInput() || result.changed;
    return result;
}
} // namespace prism::runtime
