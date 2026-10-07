#include "prism/contracts/surface_effect.hpp"
#include <cmath>
#include <stdexcept>

namespace prism::contracts {
void ValidateSurfaceEffectRegion(const SurfaceEffectRegion &region)
{
    const auto bounds = region.bounds;
    if (!std::isfinite(bounds.x) || !std::isfinite(bounds.y) || !std::isfinite(bounds.width) ||
        !std::isfinite(bounds.height) || std::abs(bounds.x) > 8192 || std::abs(bounds.y) > 8192 ||
        bounds.width <= 0 || bounds.height <= 0 || bounds.width > 8192 || bounds.height > 8192 ||
        !std::isfinite(region.corner_radius) || region.corner_radius < 0 ||
        region.corner_radius > 256 || !std::isfinite(region.blur_radius) ||
        region.blur_radius < 0 || region.blur_radius > 48) {
        throw std::invalid_argument("Invalid surface effect region");
    }
    if (region.contour) {
        ValidateContour(*region.contour);
        if (region.corner_radius != 0 || ContourBounds(*region.contour) != bounds) {
            throw std::invalid_argument("Surface effect contour bounds/radius mismatch");
        }
    }
}
} // namespace prism::contracts
