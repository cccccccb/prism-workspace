#include "scene_p.hpp"

namespace prism::runtime {

bool Scene::IsInputCleanupEvent(const contracts::WindowEvent &event) noexcept
{
    return std::holds_alternative<contracts::FocusEvent>(event) ||
           std::holds_alternative<contracts::CloseRequestedEvent>(event) ||
           std::holds_alternative<contracts::PointerCancelEvent>(event) ||
           std::holds_alternative<contracts::TouchCancelEvent>(event) ||
           std::holds_alternative<contracts::PointerLeaveEvent>(event);
}

bool Scene::HandleOwnerModalInput(const contracts::WindowEvent &event,
                                  const std::shared_ptr<const InputSnapshot> &snapshot,
                                  bool submitted)
{
    ReconcileOwnerModal();
    const auto *key = std::get_if<contracts::KeyEvent>(&event);
    if (key && key->physical_key == 0x29) {
        const auto pending =
            std::find(owner_modal_escape_.begin(), owner_modal_escape_.end(), key->source);
        if (pending != owner_modal_escape_.end()) {
            if (key->state == contracts::ButtonState::Released) {
                owner_modal_escape_.erase(pending);
            }
            return true;
        }
    }

    if (const auto *focus = std::get_if<contracts::FocusEvent>(&event)) {
        if (owner_modal_ && focus->focused) {
            const auto current =
                std::find_if(input_state_->focus.begin(), input_state_->focus.end(),
                             [focus](const InputState::Focus &entry) {
                                 return entry.seat == focus->source.seat;
                             });
            if (current == input_state_->focus.end() || !IsInteractive(current->node)) {
                MoveInputFocus(focus->source.seat, false);
            }
        }
        if (!focus->focused) {
            std::erase_if(owner_modal_escape_, [focus](contracts::InputSource source) {
                return source.seat == focus->source.seat;
            });
        }
        return false; // Focus loss still cancels streams without ending the owner task.
    }
    if (std::holds_alternative<contracts::CloseRequestedEvent>(event)) {
        return false; // A close request can be refused; the owner handles actual teardown.
    }
    if (IsInputCleanupEvent(event)) {
        return false; // Geometry-free cleanup must retire retained streams even without pixels.
    }
    if (submitted && owner_modal_epoch_ && !CurrentOwnerModalSnapshot(snapshot.get())) {
        return true;
    }
    if (!owner_modal_) {
        return false;
    }

    if (key && key->physical_key == 0x29) {
        if (key->state == contracts::ButtonState::Pressed && !key->repeat) {
            owner_modal_escape_.push_back(key->source);
            FinishOwnerModal(OwnerModalCloseReason::Escape);
        }
        return true;
    }
    // The shared hit and interactive predicates reject every target outside
    // the domain. Captured controls may still receive motion/release outside it.
    return false;
}

} // namespace prism::runtime
