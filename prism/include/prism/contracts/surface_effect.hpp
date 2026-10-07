#pragma once
#include "prism/contracts/contour.hpp"
#include "prism/contracts/types.hpp"
#include <optional>

namespace prism::contracts {
// Logical, surface-local units. Backend owns cross-surface sampling.
struct SurfaceEffectRegion {
    LogicalRect bounds;
    double corner_radius{};
    double blur_radius{};
    // When present, bounds must equal ContourBounds and corner_radius must be zero.
    // An unsupported contour is omitted, never replaced with its bounding rectangle.
    std::optional<Contour> contour{};

    bool operator==(const SurfaceEffectRegion &b) const
    {
        return bounds.x == b.bounds.x && bounds.y == b.bounds.y && bounds.width == b.bounds.width &&
               bounds.height == b.bounds.height && corner_radius == b.corner_radius &&
               blur_radius == b.blur_radius && contour == b.contour;
    }
};

// Transport boundary validation, preserving the v1 rectangle/blur limits.
// Throws std::invalid_argument before any pending request is changed.
void ValidateSurfaceEffectRegion(const SurfaceEffectRegion &region);

struct SurfaceInputRegion {
    LogicalRect bounds;
    double corner_radius{};

    bool operator==(const SurfaceInputRegion &b) const
    {
        return bounds.x == b.bounds.x && bounds.y == b.bounds.y && bounds.width == b.bounds.width &&
               bounds.height == b.bounds.height && corner_radius == b.corner_radius;
    }
};
} // namespace prism::contracts
