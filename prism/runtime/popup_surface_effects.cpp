#include "popup_surface_p.hpp"
#include "surface_effect_geometry_p.hpp"

#include <cmath>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace prism::runtime {
namespace {
SceneRegionShape EffectShape(const SnapshotNode &node)
{
    if ((node.contour_source || node.contour_spec) && !node.contour) {
        throw std::logic_error("Popup effect regions require a prepared contour");
    }
    return {contracts::NormalizeRoundedRegion({node.bounds, node.style.radius}), node.contour};
}

void CollectPopupEffects(const SceneSnapshot &snapshot, contracts::NodeId id,
                         std::vector<SceneRegionShape> &clips,
                         std::vector<contracts::SurfaceEffectRegion> &result)
{
    const auto &node = snapshot.Get(id);
    // Visual presentation scopes retain the same backdrop capability as root.
    // Their transforms are rendered locally, not converted to system effects.
    if (node.kind == Kind::Visual || !node.style.visible || node.bounds.width <= 0 ||
        node.bounds.height <= 0) {
        return;
    }

    const bool clipped = IsPopupKind(node.kind) || node.kind == Kind::ScrollView ||
                         node.style.clip || node.style.overflow == "clip";
    if (clipped) {
        clips.push_back(EffectShape(node));
    }
    if (node.style.backdrop_blur > 0) {
        std::optional<SceneRegionShape> shape = EffectShape(node);
        for (const auto &clip : clips) {
            shape = IntersectSurfaceEffectShapes(*shape, clip);
            if (!shape) {
                break;
            }
        }
        if (shape) {
            contracts::SurfaceEffectRegion effect{shape->Bounds(),
                                                  shape->contour ? 0 : shape->rounded.corner_radius,
                                                  node.style.backdrop_blur};
            if (shape->contour) {
                effect.contour = *shape->contour;
            }
            ValidatePreparedSurfaceEffect(effect);
            if (result.size() == 8) {
                throw std::length_error("Popup surface effect region limit is 8");
            }
            result.push_back(std::move(effect));
        }
    }

    for (auto child : node.children) {
        CollectPopupEffects(snapshot, child, clips, result);
    }
    if (clipped) {
        clips.pop_back();
    }
}
} // namespace

std::vector<contracts::SurfaceEffectRegion> PreparePopupEffects(const SceneSnapshot &snapshot,
                                                                contracts::NodeId root,
                                                                contracts::LogicalSize viewport)
{
    if (!std::isfinite(viewport.width) || !std::isfinite(viewport.height) || viewport.width <= 0 ||
        viewport.height <= 0 || !IsPopupKind(snapshot.Get(root).kind)) {
        throw std::invalid_argument("Popup local effects require a finite viewport and Popup root");
    }

    std::vector<contracts::SurfaceEffectRegion> result;
    std::vector<SceneRegionShape> clips{{{{0, 0, viewport.width, viewport.height}, 0}, {}}};
    CollectPopupEffects(snapshot, root, clips, result);
    return result;
}
} // namespace prism::runtime
