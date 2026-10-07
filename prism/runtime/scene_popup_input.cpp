#include "scene_p.hpp"

namespace prism::runtime {
namespace {
bool DisplayedPopupScope(const InputSnapshot &snapshot, contracts::NodeId id,
                         contracts::NodeId popup)
{
    for (std::size_t depth = 0; id && depth < snapshot.nodes.size(); ++depth) {
        if (id == popup) {
            return true;
        }
        const auto *node = snapshot.Find(id);
        if (!node) {
            break;
        }
        id = node->parent;
    }
    return false;
}
} // namespace

bool Scene::HandlePopupInput(const contracts::WindowEvent &event,
                             const std::shared_ptr<const InputSnapshot> &snapshot, bool submitted)
{
    ReconcilePopup();
    if (const auto *button = std::get_if<contracts::PointerButtonEvent>(&event)) {
        const auto pending = std::find_if(
            popup_releases_.begin(), popup_releases_.end(), [button](const PopupRelease &release) {
                return release.source == button->source && release.button == button->button;
            });
        if (pending != popup_releases_.end()) {
            if (button->state == contracts::ButtonState::Released) {
                popup_releases_.erase(pending);
            }
            return true;
        }
    }
    if (const auto *key = std::get_if<contracts::KeyEvent>(&event)) {
        if (key->physical_key == 0x29 && popup_escape_ && *popup_escape_ == key->source) {
            if (key->state == contracts::ButtonState::Released) {
                popup_escape_.reset();
            }
            return true;
        }
    }
    if (const auto *focus = std::get_if<contracts::FocusEvent>(&event);
        focus && !focus->focused && !focus->internal_transfer) {
        ClosePopup(PopupCloseReason::Unavailable);
        popup_releases_.clear();
        popup_escape_.reset();
        return false;
    }
    if (std::holds_alternative<contracts::CloseRequestedEvent>(event)) {
        ClosePopup(PopupCloseReason::Unavailable);
        return false;
    }
    // The previous frame may still show either a closed or a different popup. Never
    // interpret its coordinates as a fresh press in the replacement content.
    if (submitted && snapshot && snapshot->popup_token != PopupToken()) {
        return true;
    }
    auto *popup = Find(active_popup_);
    if (!popup) {
        return false;
    }
    if (submitted && (!snapshot || snapshot->scene != input_scene_id_)) {
        return true;
    }
    if (const auto *key = std::get_if<contracts::KeyEvent>(&event)) {
        if (key->physical_key == 0x29) {
            if (key->state == contracts::ButtonState::Pressed && !key->repeat) {
                popup_escape_ = key->source;
                BackPopup();
            }
            return true;
        }
    }
    if (const auto *key = std::get_if<contracts::KeyEvent>(&event)) {
        if (HandleMenuKey(*key, snapshot.get(), submitted)) {
            return true;
        }
    }
    if (const auto *button = std::get_if<contracts::PointerButtonEvent>(&event)) {
        if (!std::isfinite(button->position.x) || !std::isfinite(button->position.y)) {
            return true;
        }
        // Only a new press can dismiss. Captured releases belong to the control stream.
        if (button->state == contracts::ButtonState::Pressed) {
            // Dismissal uses displayed geometry. A changed action, enabled value
            // or pending scroll may deny activation without becoming an outside press.
            const auto hit = submitted
                                 ? (snapshot ? HitTest(button->position, *snapshot) : std::nullopt)
                                 : HitTest(button->position);
            const auto *target = hit ? Find(hit->node) : nullptr;
            const bool inside =
                hit && (submitted && snapshot ? DisplayedPopupScope(*snapshot, hit->node, popup->id)
                                              : target && DescendantOf(target, *popup));
            if (!inside) {
                popup_releases_.push_back({button->source, button->button});
                ClosePopup(PopupCloseReason::OutsidePress);
                return true;
            }
        }
    }
    if (const auto *wheel = std::get_if<contracts::PointerScrollEvent>(&event)) {
        const auto hit = submitted ? (snapshot ? HitTest(wheel->position, *snapshot) : std::nullopt)
                                   : HitTest(wheel->position);
        const auto *target = hit ? Find(hit->node) : nullptr;
        const bool inside =
            hit && (submitted && snapshot ? DisplayedPopupScope(*snapshot, hit->node, popup->id)
                                          : target && DescendantOf(target, *popup));
        return !inside;
    }
    // Touch support is deliberately withheld until its dismissal/capture contract exists.
    return std::holds_alternative<contracts::TouchDownEvent>(event) ||
           std::holds_alternative<contracts::TouchMotionEvent>(event) ||
           std::holds_alternative<contracts::TouchUpEvent>(event);
}
} // namespace prism::runtime
