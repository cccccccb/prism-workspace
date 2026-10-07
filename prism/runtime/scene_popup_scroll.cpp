#include "popup_surface_p.hpp"
#include "scene_p.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace prism::runtime {
void Scene::ApplyPopupScrollOffsets(SceneSnapshot &snapshot) const
{
    if (!HasPopupSurfaceAdoption()) {
        return;
    }
    for (const auto &[index, offset] : popup_surface_adoption_->scroll_offsets) {
        if (index < snapshot.nodes.size() && snapshot.nodes[index].id &&
            snapshot.nodes[index].kind == Kind::ScrollView &&
            popup_surface_adoption_->plan.input_snapshot->Find(snapshot.nodes[index].id)) {
            snapshot.nodes[index].scroll_offset = offset;
        }
    }
}

std::optional<ScrollMetrics> Scene::PopupScrollInfo(contracts::NodeId id) const
{
    if (!HasPopupSurfaceAdoption()) {
        return std::nullopt;
    }
    const auto *node = Find(id);
    const auto *popup = Find(active_popup_);
    if (!node || node->kind != Kind::ScrollView || !DescendantOf(node, *popup) ||
        !popup_surface_adoption_->plan.input_snapshot->Find(id)) {
        return std::nullopt;
    }
    const auto &layout = popup_surface_adoption_->plan.prepared->layout->Get(id);
    const auto offset = popup_surface_adoption_->scroll_offsets.find(id.index);
    if (offset == popup_surface_adoption_->scroll_offsets.end()) {
        return std::nullopt;
    }
    return ScrollMetrics{offset->second,
                         std::max(0.0, layout.scroll_content_height - layout.bounds.height),
                         layout.bounds.height, layout.scroll_content_height};
}

bool Scene::ScrollPopupTo(contracts::NodeId id, double offset)
{
    const auto info = PopupScrollInfo(id);
    if (!info || !std::isfinite(offset) || Has(dirty_, Dirty::Layout)) {
        return false;
    }
    const double next = std::clamp(offset, 0.0, info->maximum);
    if (next == info->offset) {
        return false;
    }

    auto *node = Find(id);
    PrepareInputGeometry();
    for (const auto &child : node->children) {
        if (child->kind != Kind::Visual) {
            CancelScrolledInput(*child);
        }
    }
    // Keep the semantic offset in the same Scene. Actual child geometry is
    // prepared from the cached layout; retained root coordinates stay intact.
    popup_surface_adoption_->scroll_offsets[id.index] = next;
    node->scroll_offset = next;
    ++node->revision;
    ++transaction_revision_;
    input_snapshot_dirty_ = true;
    Invalidate(Dirty::Paint | Dirty::Composite);
    RefreshInputGeometry();
    return true;
}

bool Scene::RevealPopupScrollTarget(contracts::NodeId id)
{
    if (!HasPopupSurfaceAdoption()) {
        return false;
    }
    auto *target = Find(id);
    const auto *popup = Find(active_popup_);
    if (!target || !DescendantOf(target, *popup)) {
        return false;
    }
    const auto &snapshot = *popup_surface_adoption_->trusted_input;
    const auto *shown = snapshot.Find(id);
    if (!shown || Has(dirty_, Dirty::Layout)) {
        return true;
    }

    auto bounds = shown->bounds;
    for (auto *parent = target->parent; parent && DescendantOf(parent, *popup);
         parent = parent->parent) {
        if (parent->kind != Kind::ScrollView) {
            continue;
        }
        const auto *geometry = snapshot.Find(parent->id);
        const auto info = PopupScrollInfo(parent->id);
        if (!geometry || !info) {
            continue;
        }
        double delta = 0;
        if (bounds.y < geometry->bounds.y || bounds.height > geometry->bounds.height) {
            delta = bounds.y - geometry->bounds.y;
        } else if (bounds.y + bounds.height > geometry->bounds.y + geometry->bounds.height) {
            delta = bounds.y + bounds.height - geometry->bounds.y - geometry->bounds.height;
        }
        ScrollPopupTo(parent->id, info->offset + delta);
        bounds = geometry->bounds;
    }
    return true;
}
} // namespace prism::runtime
