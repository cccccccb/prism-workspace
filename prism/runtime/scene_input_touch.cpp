#include "scene_p.hpp"

namespace prism::runtime {

void Scene::HandleTouchDown(const contracts::TouchDownEvent &event,
                            const std::shared_ptr<const InputSnapshot> &snapshot, bool submitted)
{
    if (!std::isfinite(event.position.x) || !std::isfinite(event.position.y)) {
        return;
    }
    auto &touches = input_state_->touches;
    const auto current =
        std::find_if(touches.begin(), touches.end(), [&event](const InputState::Touch &touch) {
            return touch.source == event.source && touch.contact == event.contact;
        });
    if (current != touches.end()) {
        // A duplicate Down is not a new sequence and must not retarget capture.
        return;
    }
    const auto hit = InputHit(event.position, snapshot, submitted);
    if (!hit) {
        return;
    }

    TrackInputTarget(hit->node);
    const auto gesture =
        StartGesture(hit->node, event.source, true, event.contact, event.protocol_serial,
                     event.position, event.time_ns, snapshot);
    touches.push_back({event.source, event.contact, hit->node, Find(hit->node)->action,
                       event.position, true, snapshot, submitted, gesture});
    SetInputFocus(hit->node, event.source.seat, false);
}

void Scene::HandleTouchMotion(const contracts::TouchMotionEvent &event,
                              const std::shared_ptr<const InputSnapshot> &snapshot, bool submitted)
{
    if (!std::isfinite(event.position.x) || !std::isfinite(event.position.y)) {
        return;
    }
    auto &touches = input_state_->touches;
    const auto current =
        std::find_if(touches.begin(), touches.end(), [&event](const InputState::Touch &touch) {
            return touch.source == event.source && touch.contact == event.contact;
        });
    if (current == touches.end()) {
        return;
    }

    const auto hit = InputHit(event.position, snapshot, submitted);
    current->position = event.position;
    current->inside = hit && hit->node == current->captured;
    current->snapshot = snapshot;
    current->submitted = submitted;
    MoveGesture(current->gesture, event.position, event.time_ns);
}

std::optional<Activation> Scene::HandleTouchUp(const contracts::TouchUpEvent &event,
                                               const std::shared_ptr<const InputSnapshot> &snapshot,
                                               bool submitted)
{
    auto &touches = input_state_->touches;
    const auto current =
        std::find_if(touches.begin(), touches.end(), [&event](const InputState::Touch &touch) {
            return touch.source == event.source && touch.contact == event.contact;
        });
    if (current == touches.end()) {
        return std::nullopt;
    }

    const auto hit = InputHit(current->position, snapshot, submitted);
    const auto *node = Find(current->captured);
    const bool dragged =
        FinishGesture(current->gesture, contracts::GesturePhase::End, event.time_ns);
    std::optional<Activation> activation;
    if (!dragged && hit && hit->node == current->captured && node->action == current->action &&
        !current->action.empty()) {
        activation = Activation{current->captured, current->action};
    }
    touches.erase(current);
    return activation;
}

void Scene::CancelTouchInput(contracts::InputSource source, std::uint64_t time_ns) noexcept
{
    for (const auto &touch : input_state_->touches) {
        if (touch.source == source) {
            FinishGesture(touch.gesture, contracts::GesturePhase::Cancel, time_ns);
        }
    }
    std::erase_if(input_state_->touches,
                  [source](const InputState::Touch &touch) { return touch.source == source; });
}

void Scene::RefreshTouchGeometry() noexcept
{
    for (auto &touch : input_state_->touches) {
        const auto hit = InputHit(touch.position, touch.snapshot, touch.submitted);
        touch.inside = hit && hit->node == touch.captured;
    }
}

} // namespace prism::runtime
