#include "client_application_p.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <tuple>

namespace prism::sdk {
namespace {
auto PointerKey(const contracts::InputSource &source)
{
    return std::tuple{source.seat, source.device, source.generation};
}

bool Contains(const contracts::LogicalRect &bounds, contracts::LogicalPoint point)
{
    return point.x >= bounds.x && point.y >= bounds.y && point.x < bounds.x + bounds.width &&
           point.y < bounds.y + bounds.height;
}
} // namespace

void ClientApplication::Impl::ObserveOwnerFeedbackEvent(const contracts::WindowEvent &event)
{
    if (const auto *enter = std::get_if<contracts::PointerEnterEvent>(&event)) {
        owner_feedback_pointers.insert_or_assign(PointerKey(enter->source), enter->position);
    } else if (const auto *motion = std::get_if<contracts::PointerMotionEvent>(&event)) {
        owner_feedback_pointers.insert_or_assign(PointerKey(motion->source), motion->position);
    } else if (const auto *button = std::get_if<contracts::PointerButtonEvent>(&event)) {
        if (auto found = owner_feedback_pointers.find(PointerKey(button->source));
            found != owner_feedback_pointers.end()) {
            found->second = button->position;
        }
    } else if (const auto *scroll = std::get_if<contracts::PointerScrollEvent>(&event)) {
        if (auto found = owner_feedback_pointers.find(PointerKey(scroll->source));
            found != owner_feedback_pointers.end()) {
            found->second = scroll->position;
        }
    } else if (const auto *leave = std::get_if<contracts::PointerLeaveEvent>(&event)) {
        owner_feedback_pointers.erase(PointerKey(leave->source));
    } else if (const auto *cancel = std::get_if<contracts::PointerCancelEvent>(&event)) {
        owner_feedback_pointers.erase(PointerKey(cancel->source));
    } else if (const auto *focus = std::get_if<contracts::FocusEvent>(&event);
               focus && !focus->internal_transfer) {
        owner_feedback_focus_known = true;
        if (focus->focused) {
            owner_feedback_focused_seats.insert(focus->source.seat);
        } else {
            owner_feedback_focused_seats.erase(focus->source.seat);
        }
        owner_feedback_window_focused = !owner_feedback_focused_seats.empty();
    }
}

bool ClientApplication::Impl::OwnerFeedbackPaused() const
{
    if (!owner_feedback || !scene || owner_feedback_hidden || owner_task_scope ||
        owner_feedback_wait_adoption ||
        (owner_feedback_focus_known && !owner_feedback_window_focused)) {
        return true;
    }
    const auto bounds = scene->Bounds(scene->RegionId(runtime::kOwnerFeedbackCardRegion));
    for (const auto &[source, position] : owner_feedback_pointers) {
        if (Contains(bounds, position)) {
            return true;
        }
    }
    return scene->HasFocusInRegion(runtime::kOwnerFeedbackPanelRegion);
}

void ClientApplication::Impl::ReconcileOwnerFeedback()
{
    if (!owner_feedback || !scene) {
        return;
    }
    if (owner_feedback_ui != installed_ui || !runtime::HasOwnerFeedbackPanel(*scene)) {
        ClearOwnerFeedback(true);
        return;
    }
    const auto now = scene->AnimationNowNs();
    const bool hidden = owner_task_scope.has_value();
    if (hidden != owner_feedback_hidden) {
        owner_feedback_hidden = hidden;
        owner_feedback_wait_adoption = true;
        owner_feedback_bindings.insert_or_assign("__prism_feedback_visible", !hidden);
        scene->SetBinding("__prism_feedback_visible", !hidden);
        owner_feedback_session.Pause(now);
    }
    if (hidden) {
        owner_feedback_session.Pause(now);
        return;
    }
    if (runtime::Has(scene->PendingDirty(), runtime::Dirty::Layout)) {
        UpdateOwnerFeedbackText();
        if (!owner_feedback) {
            return;
        }
    }
    if (OwnerFeedbackPaused()) {
        owner_feedback_session.Pause(now);
    } else {
        owner_feedback_session.Resume(now);
    }
    if (owner_feedback_session.Expired(now)) {
        ClearOwnerFeedback();
        InvalidateQueuedFrame();
        QueueRenderUpdate(true);
    }
}

void ClientApplication::Impl::AdoptOwnerFeedbackInput(
    const std::shared_ptr<const runtime::InputSnapshot> &input, runtime::UiLoadId ui)
{
    if (!owner_feedback || owner_feedback_hidden || owner_task_scope || !scene ||
        ui != installed_ui || ui != owner_feedback_ui || !input ||
        !scene->IsInputSnapshotAdopted(*input)) {
        return;
    }
    const auto *card = input->Find(scene->RegionId(runtime::kOwnerFeedbackCardRegion));
    if (!card || !card->visible) {
        return;
    }
    owner_feedback_wait_adoption = false;
    scene->ResolveLayout();
    const auto now = scene->AnimationNowNs();
    owner_feedback_session.Adopt(owner_feedback_generation, now, OwnerFeedbackPaused());
    ReconcileOwnerFeedback();
}

bool ClientApplication::Impl::HandleOwnerFeedbackAction(const runtime::Activation &activation)
{
    const auto &action = activation.action;
    if (!runtime::IsOwnerFeedbackReservedName(action)) {
        return false;
    }
    if (!owner_feedback || owner_feedback_hidden || owner_task_scope || !scene ||
        owner_feedback_ui != installed_ui || !owner_feedback_session.Adopted() ||
        owner_feedback_wait_adoption ||
        !scene->IsNodeInRegion(activation.node, runtime::kOwnerFeedbackPanelRegion) ||
        !scene->IsVisible(activation.node)) {
        return true;
    }
    const auto request = owner_feedback->request_id;
    if (action == runtime::OwnerFeedbackActionName(owner_feedback_generation, request, 0)) {
        ClearOwnerFeedback();
        InvalidateQueuedFrame();
        QueueRenderUpdate(true);
        return true;
    }
    for (const auto &choice : owner_feedback->actions) {
        if (action !=
            runtime::OwnerFeedbackActionName(owner_feedback_generation, request, choice.id)) {
            continue;
        }
        const contracts::OwnerFeedbackAction completed{request, choice.id};
        ClearOwnerFeedback();
        owner_feedback_action = completed;
        InvalidateQueuedFrame();
        QueueRenderUpdate(true);
        break;
    }
    return true;
}

int ClientApplication::Impl::FeedbackTimeoutMs(int timeout_ms) const noexcept
{
    const auto deadline = owner_feedback_session.Deadline();
    if (!scene || !deadline) {
        return timeout_ms;
    }
    const auto now = scene->AnimationNowNs();
    const auto remaining = *deadline > now ? *deadline - now : 0;
    const auto milliseconds = remaining / 1000000 + (remaining % 1000000 != 0);
    const auto delay = static_cast<int>(std::min<std::uint64_t>(milliseconds, INT_MAX));
    return timeout_ms < 0 ? delay : std::min(timeout_ms, delay);
}
} // namespace prism::sdk
