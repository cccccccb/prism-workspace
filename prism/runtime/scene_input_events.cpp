#include "scene_p.hpp"

namespace prism::runtime {
namespace {
constexpr std::uint32_t EnterKey = 0x28;
constexpr std::uint32_t EscapeKey = 0x29;
constexpr std::uint32_t TabKey = 0x2b;
constexpr std::uint32_t SpaceKey = 0x2c;
} // namespace

void Scene::MoveInputPointer(contracts::InputSource source, contracts::LogicalPoint point,
                             const std::shared_ptr<const InputSnapshot> &snapshot, bool submitted)
{
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
        return;
    }
    auto &pointers = input_state_->pointers;
    auto current = std::find_if(
        pointers.begin(), pointers.end(),
        [source](const InputState::Pointer &pointer) { return pointer.source == source; });
    const auto viewport = submitted && snapshot ? snapshot->viewport : viewport_;
    const bool inside = (!submitted || snapshot) &&
                        scene_detail::Inside({0, 0, viewport.width, viewport.height}, point);
    const auto hit = InputHit(point, snapshot, submitted);
    const auto id = hit ? hit->node : contracts::NodeId{};
    TrackInputTarget(id);
    if (current == pointers.end()) {
        if (inside) {
            pointers.push_back({source, id, {}, {}, point, true, snapshot, submitted});
        }
    } else {
        current->hovered = id;
        current->position = point;
        current->inside = inside;
        current->snapshot = snapshot;
        current->submitted = submitted;
    }
}

void Scene::LeaveInputPointer(contracts::InputSource source, bool cancel, std::uint64_t time_ns)
{
    for (auto &pointer : input_state_->pointers) {
        if (pointer.source != source) {
            continue;
        }
        pointer.hovered = {};
        pointer.inside = false;
        if (cancel) {
            FinishGesture(pointer.gesture, contracts::GesturePhase::Cancel, time_ns);
            pointer.gesture = 0;
            pointer.captured = {};
            pointer.action.clear();
        }
    }
}

std::optional<Activation>
Scene::HandleInputButton(const contracts::PointerButtonEvent &event,
                         const std::shared_ptr<const InputSnapshot> &snapshot, bool submitted)
{
    if (!std::isfinite(event.position.x) || !std::isfinite(event.position.y)) {
        return std::nullopt;
    }
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
        MoveInputPointer(event.source, event.position, snapshot, submitted);
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
        current->gesture =
            StartGesture(current->hovered, event.source, false, 0, event.protocol_serial,
                         event.position, event.time_ns, snapshot);
        current->captured = current->hovered;
        current->action = Find(current->captured)->action;
        SetInputFocus(current->captured, event.source.seat, false);
        return std::nullopt;
    }

    const bool dragged =
        FinishGesture(current->gesture, contracts::GesturePhase::End, event.time_ns);
    std::optional<Activation> activation;
    const auto *node = Find(current->captured);
    if (!dragged && IsInteractive(current->captured) && current->hovered == current->captured &&
        node->action == current->action) {
        if (!current->action.empty()) {
            activation = Activation{node->id, current->action};
        }
    }
    current->gesture = 0;
    current->captured = {};
    current->action.clear();
    return activation;
}

std::optional<Activation> Scene::HandleInputKey(const contracts::KeyEvent &event,
                                                const InputSnapshot *snapshot, bool submitted)
{
    if (event.repeat) {
        return std::nullopt;
    }
    const bool down = event.state == contracts::ButtonState::Pressed;
    if (down && event.physical_key == EscapeKey) {
        for (auto &pointer : input_state_->pointers) {
            if (pointer.source.seat == event.source.seat) {
                FinishGesture(pointer.gesture, contracts::GesturePhase::Cancel, event.time_ns);
                pointer.gesture = 0;
                pointer.captured = {};
                pointer.action.clear();
            }
        }
        std::erase_if(input_state_->keys, [&event](const InputState::KeyPress &key) {
            return key.source.seat == event.source.seat;
        });
        for (const auto &touch : input_state_->touches) {
            if (touch.source.seat == event.source.seat) {
                FinishGesture(touch.gesture, contracts::GesturePhase::Cancel, event.time_ns);
            }
        }
        std::erase_if(input_state_->touches, [&event](const InputState::Touch &touch) {
            return touch.source.seat == event.source.seat;
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
        MoveInputFocus(event.source.seat, event.modifiers.shift, snapshot, submitted);
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
            !IsInteractive(focus->node) || (submitted && !IsInteractive(focus->node, snapshot))) {
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
        IsInteractive(current->node) && node->action == current->action &&
        (!submitted || IsInteractive(current->node, snapshot))) {
        if (!current->action.empty()) {
            activation = Activation{node->id, current->action};
        }
    }
    keys.erase(current);
    return activation;
}

InteractionResult Scene::HandleInput(const contracts::WindowEvent &event)
{
    return DispatchInput(event, {}, false);
}

InteractionResult Scene::HandleInput(const contracts::WindowEvent &event,
                                     const std::shared_ptr<const InputSnapshot> &snapshot)
{
    return DispatchInput(event, snapshot, true);
}

InteractionResult Scene::DispatchInput(const contracts::WindowEvent &event,
                                       const std::shared_ptr<const InputSnapshot> &snapshot,
                                       bool submitted)
{
    InteractionResult result;
    if (HandleTextInput(event, result, snapshot, submitted)) {
        result.changed = ReconcileInput() || result.changed;
        return result;
    }
    if (const auto *motion = std::get_if<contracts::PointerMotionEvent>(&event)) {
        MoveInputPointer(motion->source, motion->position, snapshot, submitted);
        for (const auto &pointer : input_state_->pointers) {
            if (pointer.source == motion->source) {
                MoveGesture(pointer.gesture, motion->position, motion->time_ns);
            }
        }
    } else if (const auto *enter = std::get_if<contracts::PointerEnterEvent>(&event)) {
        MoveInputPointer(enter->source, enter->position, snapshot, submitted);
    } else if (const auto *leave = std::get_if<contracts::PointerLeaveEvent>(&event)) {
        LeaveInputPointer(leave->source, false);
    } else if (const auto *cancel = std::get_if<contracts::PointerCancelEvent>(&event)) {
        LeaveInputPointer(cancel->source, true, cancel->time_ns);
    } else if (const auto *button = std::get_if<contracts::PointerButtonEvent>(&event)) {
        result.activation = HandleInputButton(*button, snapshot, submitted);
    } else if (const auto *key = std::get_if<contracts::KeyEvent>(&event)) {
        result.activation = HandleInputKey(*key, snapshot.get(), submitted);
    } else if (const auto *down = std::get_if<contracts::TouchDownEvent>(&event)) {
        HandleTouchDown(*down, snapshot, submitted);
    } else if (const auto *motion = std::get_if<contracts::TouchMotionEvent>(&event)) {
        HandleTouchMotion(*motion, snapshot, submitted);
    } else if (const auto *up = std::get_if<contracts::TouchUpEvent>(&event)) {
        result.activation = HandleTouchUp(*up, snapshot, submitted);
    } else if (const auto *cancel = std::get_if<contracts::TouchCancelEvent>(&event)) {
        CancelTouchInput(cancel->source, cancel->time_ns);
    } else if (const auto *focus = std::get_if<contracts::FocusEvent>(&event)) {
        if (!focus->focused) {
            CancelSeatInput(focus->source.seat);
        }
    } else if (std::holds_alternative<contracts::CloseRequestedEvent>(event)) {
        result.changed = CancelInput();
    }

    result.changed = ReconcileInput() || result.changed;
    ResolveInteractionStyles();
    return result;
}
} // namespace prism::runtime
