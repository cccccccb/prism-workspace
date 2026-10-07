#include "popup_surface_p.hpp"
#include "scene_p.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace prism::runtime {
namespace {
bool ValidIdentity(const PopupSurfaceIdentity &identity)
{
    return identity.worker && identity.target && identity.lifetime &&
           identity.target == identity.lifetime && identity.configure_generation &&
           identity.submission_sequence;
}

bool SamePreparedPlan(const PopupSurfacePlan &a, const PopupSurfacePlan &b)
{
    return a.request == b.request && a.configure_generation == b.configure_generation &&
           a.body_bounds == b.body_bounds && a.surface_origin == b.surface_origin &&
           a.buffer_size.width == b.buffer_size.width &&
           a.buffer_size.height == b.buffer_size.height && a.window_geometry == b.window_geometry &&
           a.body_geometry == b.body_geometry && a.font == b.font &&
           a.display_list == b.display_list && a.input_snapshot == b.input_snapshot &&
           a.input_regions == b.input_regions && a.effect_regions == b.effect_regions;
}

void RemoveInputSubtree(InputSnapshot &snapshot, contracts::NodeId id)
{
    const auto *node = snapshot.Find(id);
    if (!node) {
        return;
    }
    const auto children = node->children;
    for (auto child : children) {
        RemoveInputSubtree(snapshot, child);
    }
    snapshot.nodes[id.index] = {};
}
} // namespace

bool Scene::HasPopupSurfaceAdoption() const noexcept
{
    return !OwnerModalToken() && popup_surface_adoption_ &&
           popup_surface_adoption_->plan.input_snapshot->owner_modal_epoch == owner_modal_epoch_ &&
           popup_surface_adoption_->plan.request.popup_token == PopupToken() &&
           popup_surface_adoption_->plan.request.active_node == active_popup_ &&
           popup_surface_adoption_->plan.request.parent_configure_generation ==
               popup_parent_configure_generation_;
}

bool Scene::IsPopupInputSnapshot(const InputSnapshot *snapshot) const noexcept
{
    return HasPopupSurfaceAdoption() && snapshot &&
           snapshot == popup_surface_adoption_->trusted_input.get();
}

std::optional<PopupSurfaceIdentity> Scene::PopupSurfaceAdoptionIdentity() const noexcept
{
    return HasPopupSurfaceAdoption() ? std::optional(popup_surface_adoption_->identity)
                                     : std::nullopt;
}

bool Scene::AdoptPopupSurface(const PopupSurfacePlan &plan, const PopupSurfaceIdentity &identity)
{
    const auto *popup = Find(active_popup_);
    if (OwnerModalToken() || !ValidIdentity(identity) || !plan.prepared ||
        !SamePreparedPlan(plan, plan.prepared->values) || !plan.prepared->layout ||
        !plan.request.source || !plan.request.source->input ||
        !CurrentOwnerModalSnapshot(plan.request.source->input.get()) || !plan.input_snapshot ||
        plan.input_snapshot->scene != 0 ||
        plan.input_snapshot->owner_modal_epoch != owner_modal_epoch_ || !popup ||
        !IsVisible(*popup) || !IsEnabled(*popup) || plan.request.scene != input_scene_id_ ||
        plan.request.popup_token != PopupToken() || plan.request.active_node != popup->id ||
        plan.request.trigger != popup->popup_anchor ||
        plan.request.parent_configure_generation != popup_parent_configure_generation_ ||
        plan.configure_generation != identity.configure_generation) {
        return false;
    }
    if (HasPopupSurfaceAdoption()) {
        const auto &old = popup_surface_adoption_->identity;
        if (identity.worker != old.worker || identity.target != old.target ||
            identity.lifetime != old.lifetime ||
            identity.configure_generation < old.configure_generation ||
            identity.submission_sequence <= old.submission_sequence) {
            return false;
        }
    }

    auto adopted = std::make_unique<PopupSurfaceAdoption>();
    adopted->identity = identity;
    adopted->plan = plan;
    auto trusted = *plan.input_snapshot;
    trusted.scene = input_scene_id_;
    adopted->trusted_input = std::make_shared<const InputSnapshot>(std::move(trusted));
    for (const auto &node : plan.prepared->layout->nodes) {
        if (node.id && node.kind == Kind::ScrollView && DescendantOf(Find(node.id), *popup)) {
            double offset = node.scroll_offset;
            if (popup_surface_adoption_ &&
                popup_surface_adoption_->plan.request.popup_token == plan.request.popup_token &&
                popup_surface_adoption_->plan.input_snapshot->Find(node.id)) {
                const auto current = popup_surface_adoption_->scroll_offsets.find(node.id.index);
                if (current != popup_surface_adoption_->scroll_offsets.end()) {
                    offset =
                        std::clamp(current->second, 0.0,
                                   std::max(0.0, node.scroll_content_height - node.bounds.height));
                }
            }
            adopted->scroll_offsets.emplace(node.id.index, offset);
        }
    }

    const bool first = !HasPopupSurfaceAdoption();
    const auto previous =
        popup_surface_adoption_ ? popup_surface_adoption_->trusted_input : nullptr;
    const bool scope_changed = first || popup_surface_adoption_->identity.configure_generation !=
                                            identity.configure_generation;
    PrepareInputGeometry();
    if (scope_changed) {
        CancelScrolledInput(*popup);
        std::erase_if(input_state_->pointers,
                      [this, popup, &previous](const InputState::Pointer &pointer) {
                          return (previous && pointer.snapshot == previous) ||
                                 DescendantOf(Find(pointer.hovered), *popup);
                      });
    }
    for (auto &pointer : input_state_->pointers) {
        if (pointer.snapshot == previous && previous) {
            pointer.snapshot = adopted->trusted_input;
        }
    }
    popup_surface_adoption_ = std::move(adopted);
    ReconcileSliderGeometry(*popup_surface_adoption_->trusted_input);
    UpdateRootSurfaceInputSnapshot(first);
    RefreshInputGeometry();
    ResolveInteractionStyles();
    if (first) {
        input_dirty_ = true;
        Invalidate(Dirty::Paint | Dirty::Composite);
    }
    return true;
}

void Scene::DropPopupSurfaceAdoption()
{
    if (!popup_surface_adoption_) {
        return;
    }
    const auto trusted = popup_surface_adoption_->trusted_input;
    for (const auto &[index, offset] : popup_surface_adoption_->scroll_offsets) {
        if (index < nodes_.size() && nodes_[index] && nodes_[index]->kind == Kind::ScrollView) {
            if (popup_surface_adoption_->plan.input_snapshot->Find(nodes_[index]->id)) {
                nodes_[index]->scroll_offset = offset;
            }
        }
    }
    if (const auto *popup = Find(popup_surface_adoption_->plan.request.active_node)) {
        CancelScrolledInput(*popup);
    }
    std::erase_if(input_state_->pointers, [&trusted](const InputState::Pointer &pointer) {
        return pointer.snapshot == trusted;
    });
    popup_surface_adoption_.reset();
    input_dirty_ = true;
    UpdateRootSurfaceInputSnapshot(true);
    Invalidate(Dirty::Layout | Dirty::Paint | Dirty::Composite);
}

bool Scene::RevokePopupSurface(const PopupSurfaceIdentity &identity)
{
    if (!popup_surface_adoption_ || popup_surface_adoption_->identity != identity) {
        return false;
    }
    DropPopupSurfaceAdoption();
    return true;
}

InteractionResult
Scene::HandlePopupSurfaceInput(const contracts::WindowEvent &event,
                               const PopupSurfaceIdentity &identity,
                               const std::shared_ptr<const InputSnapshot> &snapshot)
{
    if (!HasPopupSurfaceAdoption() || popup_surface_adoption_->identity != identity ||
        popup_surface_adoption_->plan.input_snapshot != snapshot) {
        return {};
    }
    // The descriptor remains scene=0. Only this private, adopted copy enters
    // the existing control machinery, using actual child-local geometry.
    const auto trusted = popup_surface_adoption_->trusted_input;
    return DispatchInput(event, trusted, true);
}

void Scene::UpdateRootSurfaceInputSnapshot(bool force)
{
    if (!input_snapshot_) {
        return;
    }
    const bool detached = HasPopupSurfaceAdoption();
    if (!force && !detached && !root_surface_input_snapshot_) {
        root_surface_input_source_ = input_snapshot_;
        return;
    }
    if (!force && root_surface_input_source_ == input_snapshot_ &&
        static_cast<bool>(root_surface_input_snapshot_) == detached) {
        return;
    }
    if (input_snapshot_version_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Scene input snapshot version exhausted");
    }

    auto snapshot = *input_snapshot_;
    snapshot.version = ++input_snapshot_version_;
    if (detached) {
        auto &children = snapshot.nodes[snapshot.root.index].children;
        std::erase(children, active_popup_);
        RemoveInputSubtree(snapshot, active_popup_);
        root_surface_input_snapshot_ = std::make_shared<const InputSnapshot>(std::move(snapshot));
    } else {
        input_snapshot_ = std::make_shared<const InputSnapshot>(std::move(snapshot));
        root_surface_input_snapshot_.reset();
    }
    root_surface_input_source_ = input_snapshot_;
}
} // namespace prism::runtime
