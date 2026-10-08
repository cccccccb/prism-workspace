#include "scene_p.hpp"

#include <limits>
#include <utility>

namespace prism::runtime {

std::uint64_t Scene::OwnerModalToken() const noexcept
{
    return owner_modal_ ? owner_modal_->token : 0;
}

std::uint64_t Scene::OwnerModalEpoch() const noexcept
{
    return owner_modal_epoch_;
}

bool Scene::InOwnerModalScope(const Node &node) const noexcept
{
    if (!owner_modal_) {
        return true;
    }
    const auto *root = Find(owner_modal_->root);
    return root && DescendantOf(&node, *root);
}

bool Scene::CanTraverseOwnerModal(const Node &node) const noexcept
{
    if (!owner_modal_) {
        return true;
    }
    const auto *root = Find(owner_modal_->root);
    return root && (DescendantOf(&node, *root) || DescendantOf(root, node));
}

bool Scene::CurrentOwnerModalSnapshot(const InputSnapshot *snapshot) const noexcept
{
    return snapshot && snapshot->scene == input_scene_id_ &&
           snapshot->owner_modal_epoch == owner_modal_epoch_;
}

std::optional<std::uint64_t> Scene::BeginOwnerModal(contracts::NodeId id, std::uint64_t seat)
{
    const auto *root = Find(id);
    if (owner_modal_ || !root || root->decorative || root->kind == Kind::Visual ||
        IsFloatingKind(root->kind) || !IsVisible(*root) || !IsEnabled(*root) ||
        Has(dirty_, Dirty::Layout) || owner_modal_closures_.size() >= 256 ||
        owner_modal_escape_.size() >= 256 ||
        owner_modal_epoch_ >= std::numeric_limits<std::uint64_t>::max() - 1) {
        return std::nullopt;
    }
    for (const auto *parent = root->parent; parent; parent = parent->parent) {
        if (IsFloatingKind(parent->kind)) {
            return std::nullopt;
        }
    }

    auto next = std::make_unique<OwnerModalState>();
    next->root = id;
    next->return_region = root->parent ? root->parent->id : RootId();
    next->token = owner_modal_epoch_ + 1;
    next->seat = seat;
    next->focus = input_state_->focus;
    if (const auto *popup = Find(active_popup_)) {
        for (auto &focus : next->focus) {
            if (DescendantOf(Find(focus.node), *popup)) {
                focus.node = popup->popup_anchor;
                focus.visible = true;
            }
        }
    }
    // Reserve both terminal delivery and focus restoration before changing the input domain.
    owner_modal_closures_.reserve(owner_modal_closures_.size() + 1);
    input_state_->focus.reserve(next->focus.size() + 1);
    input_state_->active.reserve(nodes_.size());

    ClosePopup(PopupCloseReason::Replaced);
    DropPopupSurfaceAdoption();
    CancelInput();
    owner_modal_epoch_ = next->token;
    owner_modal_ = std::move(next);
    input_snapshot_dirty_ = true;
    input_dirty_ = true;
    ++transaction_revision_;
    Invalidate(Dirty::Composite);
    try {
        MoveInputFocus(seat, false);
        RefreshInputStates();
        ResolveInteractionStyles();
    } catch (...) {
        // Captures and a replaced Popup cannot be replayed, but a failed Begin
        // must never leave a scope whose token was not returned to its owner.
        FinishOwnerModal(OwnerModalCloseReason::Ended);
        owner_modal_closures_.pop_back();
        throw;
    }
    return owner_modal_->token;
}

void Scene::RestoreOwnerModalFocus(const OwnerModalState &previous) noexcept
{
    // Restoration is also used after committed region removal. Keep it allocation-free;
    // the next submitted geometry governs hover and any later keyboard input.
    const auto *region = Find(previous.return_region);
    if (region && (!IsVisible(*region) || !IsEnabled(*region))) {
        region = root_.get();
    }
    const auto *old_root = Find(previous.root);
    contracts::NodeId fallback;
    for (const auto *node : nodes_) {
        if (!node || !IsInteractive(node->id) || InputAction(*node).empty() ||
            (old_root && DescendantOf(node, *old_root)) ||
            (region && !DescendantOf(node, *region))) {
            continue;
        }
        fallback = node->id;
        break;
    }
    bool initiating_seat_restored = false;
    for (const auto &saved : previous.focus) {
        const auto id = IsInteractive(saved.node) ? saved.node : fallback;
        if (id) {
            input_state_->focus.push_back({saved.seat, id, saved.visible || id != saved.node});
            TrackInputTarget(id);
            initiating_seat_restored = initiating_seat_restored || saved.seat == previous.seat;
        }
    }
    if (!initiating_seat_restored && fallback) {
        input_state_->focus.push_back({previous.seat, fallback, true});
        TrackInputTarget(fallback);
    }
}

void Scene::FinishOwnerModal(OwnerModalCloseReason reason) noexcept
{
    if (!owner_modal_) {
        return;
    }

    auto previous = std::move(owner_modal_);
    ++owner_modal_epoch_;
    owner_modal_closures_.push_back({previous->token, reason});
    CancelInput();
    RestoreOwnerModalFocus(*previous);
    input_snapshot_dirty_ = true;
    input_dirty_ = true;
    ++transaction_revision_;
    Invalidate(Dirty::Composite);
    RefreshInputStates();
}

bool Scene::EndOwnerModal(std::uint64_t expected_token)
{
    if (!expected_token || expected_token != OwnerModalToken()) {
        return false;
    }
    FinishOwnerModal(OwnerModalCloseReason::Ended);
    ResolveInteractionStyles();
    return true;
}

bool Scene::SetOwnerModalInputReady(std::uint64_t expected_token, bool ready)
{
    ReconcileOwnerModal();
    if (!expected_token || expected_token != OwnerModalToken()) {
        return false;
    }
    if (owner_modal_->input_ready == ready) {
        return true;
    }
    if (ready) {
        owner_modal_->input_ready = true;
        return true;
    }

    // A later release cannot commit a stream started before the gate closed.
    // Preserve the in-scope keyboard focus and its original restoration record.
    input_state_->active.reserve(input_state_->active.size() + input_state_->focus.size());
    auto focus = std::move(input_state_->focus);
    try {
        CancelInput();
    } catch (...) {
        input_state_->focus = std::move(focus);
        for (const auto &saved : input_state_->focus) {
            TrackInputTarget(saved.node);
        }
        RefreshInputStates();
        throw;
    }
    input_state_->focus = std::move(focus);
    for (const auto &saved : input_state_->focus) {
        TrackInputTarget(saved.node);
    }
    owner_modal_->input_ready = false;
    RefreshInputStates();
    ResolveInteractionStyles();

    return true;
}

std::optional<std::uint64_t> Scene::RefreshOwnerModal(std::uint64_t expected_token)
{
    ReconcileOwnerModal();
    if (!expected_token || expected_token != OwnerModalToken() || Has(dirty_, Dirty::Layout) ||
        owner_modal_epoch_ >= std::numeric_limits<std::uint64_t>::max() - 1) {
        return std::nullopt;
    }

    // Cancel pointer/key streams while retaining the in-panel text focus and
    // original owner focus restoration record. No intermediate domain is exposed.
    input_state_->active.reserve(input_state_->active.size() + input_state_->focus.size());
    auto focus = std::move(input_state_->focus);
    try {
        CancelInput();
    } catch (...) {
        input_state_->focus = std::move(focus);
        for (const auto &saved : input_state_->focus) {
            TrackInputTarget(saved.node);
        }
        RefreshInputStates();
        throw;
    }
    input_state_->focus = std::move(focus);
    // CancelInput removes inactive targets while focus is temporarily absent.
    // Restore their tracking before recomputing focus presentation state.
    for (const auto &saved : input_state_->focus) {
        TrackInputTarget(saved.node);
    }
    owner_modal_->token = ++owner_modal_epoch_;
    input_snapshot_dirty_ = true;
    input_dirty_ = true;
    ++transaction_revision_;
    Invalidate(Dirty::Composite);
    RefreshInputStates();

    return owner_modal_->token;
}

void Scene::ReconcileOwnerModal() noexcept
{
    if (!owner_modal_) {
        return;
    }
    const auto *root = Find(owner_modal_->root);
    if (!root || !IsVisible(*root) || !IsEnabled(*root)) {
        FinishOwnerModal(OwnerModalCloseReason::Unavailable);
    }
}

std::vector<OwnerModalClosure> Scene::TakeOwnerModalClosures()
{
    auto result = owner_modal_closures_;
    // Retain the reservation for an active scope's allocation-free terminal event.
    owner_modal_closures_.clear();
    return result;
}

} // namespace prism::runtime
