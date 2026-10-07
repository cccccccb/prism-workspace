#include "prism/runtime/layout_engine.hpp"
#include "scene_p.hpp"
#include <stdexcept>

namespace prism::runtime {
std::optional<TextLayoutInfo> Scene::TextLayoutInRegion(std::string_view region) const
{
    const auto *node = Find(RegionId(region));
    if (!node || node->kind != Kind::Box || !IsVisible(*node) || node->children.size() != 1 ||
        node->children.front()->kind != Kind::Text) {
        return std::nullopt;
    }

    const auto height = node->parent && node->parent->kind == Kind::ScrollView
                            ? node->parent->bounds.height
                            : node->bounds.height;
    return TextLayoutInfo{node->bounds.width, height, node->children.front()->style.font_size};
}

void Scene::ApplyResolvedLayout(const SceneSnapshot &snapshot)
{
    for (const auto &item : snapshot.nodes) {
        if (auto *node = Find(item.id)) {
            node->bounds = item.bounds;
            node->scroll_offset = item.scroll_offset;
            node->scroll_content_height = item.scroll_content_height;
            node->shaped = item.shaped;
        }
    }
    ++layout_count_;
    input_dirty_ = true;
}

void Scene::ResolveLayout()
{
    ReconcileOwnerModal();
    ReconcilePopup();
    ResolveInteractionStyles();
    if (!root_ || !scene_detail::ValidSize(viewport_)) {
        throw std::logic_error("Measurement requires a valid Scene viewport");
    }
    if (!Has(dirty_, Dirty::Layout)) {
        return;
    }

    if (hit_geometry_dirty_) {
        PrepareInputGeometry();
    }
    auto snapshot = CaptureResolvedSnapshot();
    LayoutEngine::Compute(snapshot, viewport_, shaper_);
    ApplyResolvedLayout(snapshot);
    dirty_ =
        static_cast<Dirty>(static_cast<unsigned>(dirty_) & ~static_cast<unsigned>(Dirty::Layout)) |
        Dirty::Paint;
}
} // namespace prism::runtime
