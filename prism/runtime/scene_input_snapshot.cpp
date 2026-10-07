#include "prism/contracts/rounded_region.hpp"
#include "scene_contour_p.hpp"
#include "scene_p.hpp"
#include <atomic>
#include <limits>
#include <stdexcept>

namespace prism::runtime {

std::uint64_t Scene::NextInputSceneId()
{
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    while (value < std::numeric_limits<std::uint64_t>::max()) {
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) {
            return value;
        }
    }
    throw std::overflow_error("Scene input identity exhausted");
}

std::shared_ptr<const InputSnapshot> Scene::InputGeometry() const noexcept
{
    return root_surface_input_snapshot_ ? root_surface_input_snapshot_ : input_snapshot_;
}

std::shared_ptr<const InputSnapshot> Scene::CaptureInputSnapshot()
{
    if (!root_ || !scene_detail::ValidSize(viewport_) || Has(dirty_, Dirty::Layout)) {
        throw std::logic_error("Input snapshot requires resolved layout");
    }

    input_snapshot_dirty_ = input_snapshot_dirty_ || hit_geometry_dirty_;
    UpdateInputSnapshot();
    return InputGeometry();
}

void Scene::UpdateInputSnapshot()
{
    if (!input_snapshot_dirty_) {
        UpdateRootSurfaceInputSnapshot();
        return;
    }

    InputSnapshot snapshot;
    snapshot.scene = input_scene_id_;
    snapshot.popup_token = PopupToken();
    snapshot.root = root_->id;
    snapshot.viewport = viewport_;
    snapshot.nodes.resize(nodes_.size());
    for (const auto *node : nodes_) {
        if (!node || node->decorative || node->kind == Kind::Visual) {
            continue;
        }
        auto &item = snapshot.nodes[node->id.index];
        item.id = node->id;
        item.parent = node->parent ? node->parent->id : contracts::NodeId{};
        item.bounds = node->bounds;
        item.radius = node->contour_source || node->contour_recipe ? 0 : node->style.radius;
        item.contour = node->contour;
        item.scroll_offset = node->scroll_offset;
        if (node->kind == Kind::Slider) {
            item.slider_track = SliderTrack(*node);
        }
        item.clip = (IsPopupKind(node->kind) || node->kind == Kind::ScrollView) ||
                    node->style.clip || node->style.overflow == "clip";
        item.visible = IsVisible(*node);
        item.enabled = IsEnabled(*node);
        item.interactive = !InputAction(*node).empty() ||
                           (node->kind == Kind::InteractionTarget ||
                            (IsPopupKind(node->kind) || node->kind == Kind::ScrollView));
        item.action = InputAction(*node);
        item.gesture = node->gesture;
        for (const auto &child : node->children) {
            if (!child->decorative && child->kind != Kind::Visual) {
                item.children.push_back(child->id);
            }
        }
    }
    if (input_snapshot_ && input_snapshot_->popup_token == snapshot.popup_token &&
        input_snapshot_->root == snapshot.root && input_snapshot_->viewport == snapshot.viewport &&
        input_snapshot_->nodes == snapshot.nodes) {
        input_snapshot_dirty_ = false;
        UpdateRootSurfaceInputSnapshot();
        return;
    }

    if (input_snapshot_version_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Scene input snapshot version exhausted");
    }
    snapshot.version = ++input_snapshot_version_;
    input_snapshot_ = std::make_shared<const InputSnapshot>(std::move(snapshot));
    input_snapshot_dirty_ = false;
    UpdateRootSurfaceInputSnapshot();
}

bool Scene::ApplyInputSnapshot(const std::shared_ptr<const InputSnapshot> &snapshot)
{
    if (!snapshot || snapshot->scene != input_scene_id_ ||
        snapshot->version <= applied_input_version_) {
        return false;
    }

    PrepareInputGeometry();
    for (auto &pointer : input_state_->pointers) {
        if (pointer.submitted && !IsPopupInputSnapshot(pointer.snapshot.get())) {
            pointer.snapshot = snapshot;
        }
    }
    for (auto &touch : input_state_->touches) {
        if (touch.submitted && !IsPopupInputSnapshot(touch.snapshot.get())) {
            touch.snapshot = snapshot;
        }
    }
    ReconcileSliderGeometry(*snapshot);
    applied_input_version_ = snapshot->version;
    const auto pixels = pixels_revision_;
    RefreshInputGeometry();
    ResolveInteractionStyles();
    return pixels != pixels_revision_;
}

bool Scene::IsInteractive(contracts::NodeId id, const InputSnapshot *snapshot) const
{
    const auto *item =
        snapshot && snapshot->scene == input_scene_id_ ? snapshot->Find(id) : nullptr;
    const auto *node = Find(id);
    if (HasPopupSurfaceAdoption() && !IsPopupInputSnapshot(snapshot) && node &&
        DescendantOf(node, *Find(active_popup_))) {
        return false;
    }
    return snapshot && snapshot->popup_token == PopupToken() && item && item->visible &&
           item->enabled && item->interactive && IsInteractive(id) &&
           item->action == InputAction(*node) && item->gesture == node->gesture &&
           CurrentScrollGeometry(*node, *snapshot);
}

std::optional<HitResult> Scene::Hit(const InputSnapshotNode &node, contracts::LogicalPoint point,
                                    const InputSnapshot &snapshot) const
{
    if (!node.visible || !node.enabled) {
        return std::nullopt;
    }
    const bool rounded_inside =
        SceneShapeContains({{node.bounds, node.radius}, node.contour}, point);
    if (node.clip && !rounded_inside) {
        return std::nullopt;
    }

    for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
        const auto *child = snapshot.Find(*it);
        if (child) {
            if (auto hit = Hit(*child, point, snapshot)) {
                return hit;
            }
        }
    }
    if (rounded_inside && node.interactive) {
        return HitResult{node.id, {point.x - node.bounds.x, point.y - node.bounds.y}};
    }
    return std::nullopt;
}

std::optional<HitResult> Scene::HitTest(contracts::LogicalPoint point,
                                        const InputSnapshot &snapshot) const
{
    const auto *root = snapshot.Find(snapshot.root);
    if (snapshot.scene != input_scene_id_ || !root || !std::isfinite(point.x) ||
        !std::isfinite(point.y) ||
        !scene_detail::Inside({0, 0, snapshot.viewport.width, snapshot.viewport.height}, point)) {
        return std::nullopt;
    }
    return Hit(*root, point, snapshot);
}

std::optional<HitResult> Scene::InputHit(contracts::LogicalPoint point,
                                         const std::shared_ptr<const InputSnapshot> &snapshot,
                                         bool submitted) const
{
    if (submitted) {
        const auto hit = snapshot ? HitTest(point, *snapshot) : std::nullopt;
        return hit && IsInteractive(hit->node, snapshot.get()) ? hit : std::nullopt;
    }
    return scene_detail::Inside({0, 0, viewport_.width, viewport_.height}, point) ? HitTest(point)
                                                                                  : std::nullopt;
}

} // namespace prism::runtime
