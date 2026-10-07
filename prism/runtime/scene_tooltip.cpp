#include "scene_p.hpp"

#include <exception>
#include <limits>

namespace prism::runtime {
namespace {
std::uint64_t AddDelay(std::uint64_t now, double milliseconds)
{
    const auto delay = static_cast<std::uint64_t>(milliseconds * 1'000'000);
    return now > std::numeric_limits<std::uint64_t>::max() - delay
               ? std::numeric_limits<std::uint64_t>::max()
               : now + delay;
}
} // namespace

contracts::NodeId Scene::TooltipForAnchor(contracts::NodeId id) const
{
    const auto *anchor = Find(id);
    if (!anchor || !IsInteractive(id, input_snapshot_.get())) {
        return {};
    }
    for (const auto &child : root_->children) {
        if (child->kind == Kind::Tooltip && child->tooltip_for == anchor->action &&
            child->style.visible && child->style.FitsViewport(viewport_) && IsEnabled(*child)) {
            return child->id;
        }
    }
    return {};
}

bool Scene::SetTooltipPresentation(contracts::NodeId tooltip, contracts::NodeId anchor) noexcept
{
    if (active_tooltip_ == tooltip && tooltip_anchor_ == anchor) {
        return false;
    }
    auto &state = *tooltip_state_;
    state.mutating = true;
    if (auto *old = Find(active_tooltip_)) {
        ++old->revision;
    }
    active_tooltip_ = tooltip;
    tooltip_anchor_ = anchor;
    if (auto *node = Find(tooltip)) {
        ++node->revision;
    }
    ++transaction_revision_;
    Invalidate(Dirty::Layout | Dirty::Paint | Dirty::Composite);
    state.mutating = false;
    return true;
}

bool Scene::HideTooltip(bool suppress_current) noexcept
{
    auto &state = *tooltip_state_;
    if (suppress_current) {
        state.blocked = state.candidate;
    }
    state.candidate = {};
    state.deadline.reset();
    return SetTooltipPresentation({}, {});
}

void Scene::InvalidateTooltipGeometry() noexcept
{
    if (!tooltip_state_ || tooltip_state_->mutating) {
        return;
    }
    // A dirty layout fences geometry, but is not a new hover. Keep the
    // suppression identity and pending absolute deadline through preparation.
    if (active_tooltip_) {
        tooltip_state_->deadline = tooltip_state_->last_now;
    }
    SetTooltipPresentation({}, {});
}

std::optional<std::uint64_t> Scene::NextTooltipDeadlineNs() const noexcept
{
    return !Has(dirty_, Dirty::Layout) && input_snapshot_ &&
                   IsInputSnapshotAdopted(*input_snapshot_)
               ? tooltip_state_->deadline
               : std::nullopt;
}

bool Scene::ReconcileTooltipAvailability() noexcept
{
    const auto *tooltip = Find(active_tooltip_);
    const auto *anchor = Find(tooltip_anchor_);
    if (active_tooltip_ &&
        (!tooltip || !anchor || !tooltip_state_->owner_available || OwnerModalToken() ||
         PopupToken() || !IsVisible(*tooltip) || !IsEnabled(*tooltip) ||
         !IsInteractive(tooltip_anchor_) || tooltip->tooltip_for != anchor->action)) {
        return HideTooltip(true);
    }
    return false;
}

bool Scene::ReconcileTooltip(std::uint64_t now)
{
    auto &state = *tooltip_state_;
    now = std::max(now, state.last_now);
    state.last_now = now;
    const bool unavailable = ReconcileTooltipAvailability();
    if (!root_ || !state.owner_available || OwnerModalToken() || PopupToken() ||
        !input_state_->keys.empty() || !input_state_->touches.empty()) {
        return HideTooltip() || unavailable;
    }
    // Geometry changes wait for the worker. Opening this passive layer itself
    // prepares another snapshot; preserve its state until that adoption arrives.
    if (Has(dirty_, Dirty::Layout) || !input_snapshot_ ||
        !IsInputSnapshotAdopted(*input_snapshot_)) {
        return unavailable;
    }

    TooltipState::Candidate candidate;
    for (const auto &focus : input_state_->focus) {
        if (!state.prefer_keyboard) {
            break;
        }
        if (!focus.visible) {
            continue;
        }
        if (const auto tooltip = TooltipForAnchor(focus.node)) {
            candidate = {tooltip, focus.node, {focus.seat, 0}, true};
            break;
        }
    }
    for (const auto &pointer : input_state_->pointers) {
        if (state.prefer_keyboard) {
            break;
        }
        if (!pointer.inside || pointer.captured || pointer.gesture || !pointer.hovered ||
            (pointer.submitted &&
             (!pointer.snapshot || !IsInputSnapshotAdopted(*pointer.snapshot)))) {
            continue;
        }
        if (const auto tooltip = TooltipForAnchor(pointer.hovered)) {
            candidate = {tooltip, pointer.hovered, pointer.source, false};
            break;
        }
    }
    if (!candidate.tooltip) {
        const bool still_hovered =
            std::any_of(input_state_->pointers.begin(), input_state_->pointers.end(),
                        [&state](const InputState::Pointer &pointer) {
                            return pointer.inside && pointer.hovered == state.blocked.anchor &&
                                   pointer.source == state.blocked.source;
                        });
        if (!still_hovered) {
            state.blocked = {};
        }
        return HideTooltip() || unavailable;
    }
    if (candidate == state.blocked) {
        return HideTooltip() || unavailable;
    }
    const auto bounds = input_snapshot_->Find(candidate.anchor)->bounds;
    const auto delay = candidate.keyboard ? 0 : Find(candidate.tooltip)->tooltip_delay_ms;
    if (candidate != state.candidate || bounds != state.candidate_bounds ||
        delay != state.candidate_delay_ms) {
        const bool hidden = SetTooltipPresentation({}, {});
        state.candidate = candidate;
        state.candidate_bounds = bounds;
        state.candidate_delay_ms = delay;
        state.blocked = {};
        state.deadline = AddDelay(now, delay);
        if (now < *state.deadline) {
            return hidden || unavailable;
        }
    }
    if (!state.deadline || now < *state.deadline) {
        return unavailable;
    }

    state.deadline.reset();
    try {
        ValidateTooltipTree();
    } catch (const std::exception &) {
        return HideTooltip(true) || unavailable;
    }
    return SetTooltipPresentation(candidate.tooltip, candidate.anchor) || unavailable;
}
} // namespace prism::runtime
