#include "prism/runtime/scene_snapshot.hpp"
#include "scene_contour_p.hpp"
#include "scene_p.hpp"

#include <stdexcept>

namespace prism::runtime {
void Scene::ValidateScrollTree(const Node &node) const
{
    if (!node.scroll_part.empty() &&
        (node.kind != Kind::Visual || !node.parent || node.parent->kind != Kind::ScrollView ||
         !node.children.empty() || node.style.width <= 0 || !node.slider_part.empty())) {
        throw std::invalid_argument("Scroll part requires a direct leaf Visual with a width");
    }
    for (const auto &binding : node.bindings) {
        if (binding.target == DslProperty::ScrollPart) {
            throw std::invalid_argument("Scroll part identity cannot be bound");
        }
    }
    for (const auto &ref : node.theme_refs) {
        if (ref.target == DslProperty::ScrollPart) {
            throw std::invalid_argument("Scroll part identity cannot come from a theme");
        }
    }
    if (node.kind == Kind::ScrollView) {
        if (!node.action.empty() || node.style.padding != 0 || node.style.padding_x > 0 ||
            node.style.padding_y > 0 || !node.region.empty()) {
            throw std::invalid_argument("ScrollView padding belongs to its content");
        }
        unsigned contents = 0;
        std::set<std::string> parts;
        for (const auto &child : node.children) {
            if (child->kind == Kind::Visual) {
                if (child->scroll_part.empty() || !parts.insert(child->scroll_part).second) {
                    throw std::invalid_argument("ScrollView has invalid or duplicate parts");
                }
            } else {
                ++contents;
            }
        }
        if (contents != 1) {
            throw std::invalid_argument("ScrollView requires exactly one flow content child");
        }
        for (const auto *parent = node.parent; parent; parent = parent->parent) {
            if (parent->kind == Kind::Visual) {
                throw std::invalid_argument("ScrollView cannot be decorative");
            }
        }
    }
    for (const auto &child : node.children) {
        ValidateScrollTree(*child);
    }
}

std::optional<ScrollMetrics> Scene::ScrollInfo(contracts::NodeId id) const
{
    if (auto local = PopupScrollInfo(id)) {
        return local;
    }
    const auto *node = Find(id);
    if (!node || node->kind != Kind::ScrollView) {
        return std::nullopt;
    }
    return ScrollMetrics{node->scroll_offset,
                         std::max(0.0, node->scroll_content_height - node->bounds.height),
                         node->bounds.height, node->scroll_content_height};
}

bool Scene::DescendantOf(const Node *node, const Node &parent) noexcept
{
    for (; node; node = node->parent) {
        if (node == &parent) {
            return true;
        }
    }
    return false;
}

void Scene::TranslateScrolledTree(Node &node, double delta) noexcept
{
    node.bounds.y += delta;
    for (auto &child : node.children) {
        TranslateScrolledTree(*child, delta);
    }
}

void Scene::PrepareScrolledContours(Node &node, SceneRegionPlacement &placement) const
{
    if (node.contour_source && IsVisible(node) && node.bounds.width > 0 && node.bounds.height > 0) {
        placement.contours.emplace(
            node.id.index,
            PlaceSceneContour(*node.contour_source,
                              {node.bounds.x, node.bounds.y + placement.delta}, node.contour));
    }
    for (auto &child : node.children) {
        PrepareScrolledContours(*child, placement);
    }
}

bool Scene::ScrollTo(contracts::NodeId id, double offset)
{
    if (PopupScrollInfo(id)) {
        return ScrollPopupTo(id, offset);
    }
    auto *node = Find(id);
    if (!node || node->kind != Kind::ScrollView || !std::isfinite(offset) ||
        Has(dirty_, Dirty::Layout)) {
        return false;
    }
    const double next =
        std::clamp(offset, 0.0, std::max(0.0, node->scroll_content_height - node->bounds.height));
    if (next == node->scroll_offset) {
        return false;
    }
    const double delta = node->scroll_offset - next;
    const auto *popup = Find(active_popup_);
    SceneRegionPlacement placement;
    placement.scroll_root = node->id;
    placement.delta = delta;
    placement.close_popup = popup && DescendantOf(Find(popup->popup_anchor), *node);
    std::vector<contracts::SurfaceInputRegion> prepared_input;
    try {
        for (auto &child : node->children) {
            if (child->kind != Kind::Visual) {
                PrepareScrolledContours(*child, placement);
            }
        }
        prepared_input = PrepareScrolledRegions(placement);
        PrepareInputGeometry();
        if (placement.close_popup) {
            input_state_->active.reserve(input_state_->active.size() +
                                         input_state_->pointers.size() + 1);
            input_state_->focus.reserve(input_state_->focus.size() + 1);
        }
    } catch (const std::exception &) {
        return false;
    }

    // The whole prospective geometry is valid. Install its prepared mask before
    // input/popup reconciliation so any later policy invalidation stays dirty.
    input_regions_.swap(prepared_input);
    input_dirty_ = false;
    if (placement.close_popup) {
        ClosePopup(PopupCloseReason::Unavailable);
    }
    for (auto &child : node->children) {
        if (child->kind != Kind::Visual) {
            CancelScrolledInput(*child);
            TranslateScrolledTree(*child, delta);
        }
    }
    for (auto &[index, contour] : placement.contours) {
        nodes_[index]->contour = std::move(contour);
    }
    node->scroll_offset = next;
    ++node->revision;
    ++transaction_revision_;
    input_snapshot_dirty_ = true;
    hit_geometry_dirty_ = true;
    Invalidate(Dirty::Paint | Dirty::Composite);
    RefreshInputGeometry();
    return true;
}

bool Scene::CurrentScrollGeometry(const Node &node, const InputSnapshot &snapshot) const
{
    for (const auto *parent = node.parent; parent; parent = parent->parent) {
        if (parent->kind == Kind::ScrollView) {
            const auto *item = snapshot.Find(parent->id);
            const auto local =
                IsPopupInputSnapshot(&snapshot) ? PopupScrollInfo(parent->id) : std::nullopt;
            const double current = local ? local->offset : parent->scroll_offset;
            if (!item || item->scroll_offset != current) {
                return false;
            }
        }
    }
    return true;
}

void Scene::ApplyScrollVisuals(SceneSnapshot &snapshot) const
{
    for (const auto *node : nodes_) {
        if (!node || node->kind != Kind::ScrollView || !IsVisible(*node)) {
            continue;
        }
        const double maximum = std::max(0.0, node->scroll_content_height - node->bounds.height);
        for (const auto &child : node->children) {
            if (child->kind != Kind::Visual) {
                continue;
            }
            auto &part = snapshot.Get(child->id);
            part.style.visible = part.style.visible && maximum > 0;
            const double inset = child->style.inset;
            const double track = std::max(0.0, node->bounds.height - 2 * inset);
            double height = track;
            double y = node->bounds.y + inset;
            if (child->scroll_part == "thumb" && maximum > 0) {
                height =
                    std::min(track, std::max(child->style.height, track * node->bounds.height /
                                                                      node->scroll_content_height));
                y += (track - height) * node->scroll_offset / maximum;
            }
            const double width = std::min(child->style.width, node->bounds.width);
            part.bounds = {node->bounds.x + std::max(0.0, node->bounds.width - inset - width), y,
                           width, height};
        }
    }
}
} // namespace prism::runtime
