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
    return input_snapshot_;
}

std::shared_ptr<const InputSnapshot> Scene::CaptureInputSnapshot()
{
    if (!root_ || !scene_detail::ValidSize(viewport_) || Has(dirty_, Dirty::Layout)) {
        throw std::logic_error("Input snapshot requires resolved layout");
    }

    input_snapshot_dirty_ = input_snapshot_dirty_ || hit_geometry_dirty_;
    UpdateInputSnapshot();
    return input_snapshot_;
}

void Scene::UpdateInputSnapshot()
{
    if (!input_snapshot_dirty_) {
        return;
    }

    InputSnapshot snapshot;
    snapshot.scene = input_scene_id_;
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
        item.radius = node->style.radius;
        item.clip = node->style.clip || node->style.overflow == "clip";
        item.visible = IsVisible(*node);
        item.enabled = IsEnabled(*node);
        item.interactive = !node->action.empty() || node->kind == Kind::InteractionTarget;
        item.action = node->action;
        item.gesture = node->gesture;
        for (const auto &child : node->children) {
            if (!child->decorative && child->kind != Kind::Visual) {
                item.children.push_back(child->id);
            }
        }
    }
    if (input_snapshot_ && input_snapshot_->root == snapshot.root &&
        input_snapshot_->viewport == snapshot.viewport &&
        input_snapshot_->nodes == snapshot.nodes) {
        input_snapshot_dirty_ = false;
        return;
    }

    if (input_snapshot_ && input_snapshot_->version == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Scene input snapshot version exhausted");
    }
    snapshot.version = input_snapshot_ ? input_snapshot_->version + 1 : 1;
    input_snapshot_ = std::make_shared<const InputSnapshot>(std::move(snapshot));
    input_snapshot_dirty_ = false;
}

bool Scene::ApplyInputSnapshot(const std::shared_ptr<const InputSnapshot> &snapshot)
{
    if (!snapshot || snapshot->scene != input_scene_id_ ||
        snapshot->version <= applied_input_version_) {
        return false;
    }

    PrepareInputGeometry();
    for (auto &pointer : input_state_->pointers) {
        if (pointer.submitted) {
            pointer.snapshot = snapshot;
        }
    }
    for (auto &touch : input_state_->touches) {
        if (touch.submitted) {
            touch.snapshot = snapshot;
        }
    }
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
    return item && item->visible && item->enabled && item->interactive && IsInteractive(id) &&
           item->action == node->action && item->gesture == node->gesture;
}

std::optional<HitResult> Scene::Hit(const InputSnapshotNode &node, contracts::LogicalPoint point,
                                    const InputSnapshot &snapshot) const
{
    if (!node.visible || !node.enabled) {
        return std::nullopt;
    }
    const bool inside = scene_detail::Inside(node.bounds, point);
    if (!inside && node.clip) {
        return std::nullopt;
    }

    const double radius = std::min({node.radius, node.bounds.width / 2, node.bounds.height / 2});
    bool rounded_inside = inside;
    if (radius > 0 && inside) {
        const double cx =
            std::clamp(point.x, node.bounds.x + radius, node.bounds.x + node.bounds.width - radius);
        const double cy = std::clamp(point.y, node.bounds.y + radius,
                                     node.bounds.y + node.bounds.height - radius);
        rounded_inside =
            (point.x - cx) * (point.x - cx) + (point.y - cy) * (point.y - cy) <= radius * radius;
    }
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
