#pragma once

#include "prism/contracts/surface_effect.hpp"
#include "scene_contour_p.hpp"

#include <optional>

namespace prism::runtime {
contracts::LogicalRect IntersectSurfaceRegionBounds(contracts::LogicalRect, contracts::LogicalRect);
bool SameSurfaceRegionShape(const SceneRegionShape &, const SceneRegionShape &);
// Reject a partial intersection that one existing contour/rounded descriptor
// cannot express exactly; a bounding rectangle must never enlarge the clip.
std::optional<SceneRegionShape> IntersectSurfaceEffectShapes(const SceneRegionShape &,
                                                             const SceneRegionShape &);
void ValidatePreparedSurfaceEffect(const contracts::SurfaceEffectRegion &);
} // namespace prism::runtime
