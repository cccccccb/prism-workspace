#pragma once

#include "prism/contracts/rounded_region.hpp"
#include "prism/runtime/scene_snapshot.hpp"
#include <memory>
#include <unordered_map>

namespace prism::runtime {

contracts::PanelContourSpec ResolveContourRecipe(const AttachedPanelRecipe &,
                                                 const contracts::ThemeSnapshot *);
void PrepareAttachedPanelContours(SceneSnapshot &);

struct SceneRegionShape {
    contracts::SurfaceInputRegion rounded;
    std::shared_ptr<const contracts::Contour> contour;

    contracts::LogicalRect Bounds() const
    {
        return contour ? contracts::ContourBounds(*contour) : rounded.bounds;
    }
};

// Prospective scroll geometry only. Consumers borrow this during validation;
// it never replaces live bounds or publishes a temporary input snapshot.
struct SceneRegionPlacement {
    contracts::NodeId scroll_root;
    double delta{};
    bool close_popup{};
    std::unordered_map<std::uint32_t, std::shared_ptr<const contracts::Contour>> contours;
};

std::shared_ptr<const contracts::Contour>
PlaceSceneContour(const contracts::Contour &, contracts::LogicalPoint origin,
                  const std::shared_ptr<const contracts::Contour> &previous);
void PrepareSceneContours(SceneSnapshot &);
bool SceneShapeContains(const SceneRegionShape &, contracts::LogicalPoint);

} // namespace prism::runtime
