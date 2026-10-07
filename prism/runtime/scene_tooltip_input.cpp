#include "scene_p.hpp"

namespace prism::runtime {
void Scene::ObserveTooltipInput(const contracts::WindowEvent &event,
                                const std::shared_ptr<const InputSnapshot> &snapshot,
                                bool submitted)
{
    auto &state = *tooltip_state_;
    if (const auto *focus = std::get_if<contracts::FocusEvent>(&event)) {
        state.owner_available = focus->focused;
        if (!focus->focused) {
            HideTooltip();
            state.blocked = {};
        }
        return;
    }
    if (std::holds_alternative<contracts::CloseRequestedEvent>(event)) {
        state.owner_available = false;
        HideTooltip();
        return;
    }
    if (std::holds_alternative<contracts::PointerLeaveEvent>(event) ||
        std::holds_alternative<contracts::PointerCancelEvent>(event)) {
        HideTooltip();
        state.blocked = {};
        return;
    }
    if (submitted && (!snapshot || !IsInputSnapshotAdopted(*snapshot))) {
        return;
    }
    if (std::holds_alternative<contracts::PointerEnterEvent>(event)) {
        state.owner_available = true;
        state.blocked = {};
        state.prefer_keyboard = false;
    }
    if (std::holds_alternative<contracts::PointerMotionEvent>(event)) {
        state.prefer_keyboard = false;
    }
    if (const auto *button = std::get_if<contracts::PointerButtonEvent>(&event);
        button && button->state == contracts::ButtonState::Pressed) {
        HideTooltip(true);
    }
    if (std::holds_alternative<contracts::PointerScrollEvent>(event) ||
        std::holds_alternative<contracts::TextInputEvent>(event)) {
        HideTooltip(true);
    }
    if (const auto *key = std::get_if<contracts::KeyEvent>(&event);
        key && key->state == contracts::ButtonState::Pressed && !key->repeat) {
        const bool tab = key->physical_key == 0x2b;
        state.prefer_keyboard = true;
        HideTooltip(!tab);
        if (tab) {
            state.blocked = {};
        }
    }
}
} // namespace prism::runtime
