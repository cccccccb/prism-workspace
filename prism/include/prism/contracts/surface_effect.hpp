#pragma once
#include "prism/contracts/types.hpp"
namespace prism::contracts {
// Logical, surface-local units. Backend owns cross-surface sampling.
struct SurfaceEffectRegion {
    LogicalRect bounds;
    double corner_radius{};
    double blur_radius{};
    bool operator==(const SurfaceEffectRegion& b) const {
        return bounds.x==b.bounds.x && bounds.y==b.bounds.y && bounds.width==b.bounds.width &&
            bounds.height==b.bounds.height && corner_radius==b.corner_radius && blur_radius==b.blur_radius;
    }
};
struct SurfaceInputRegion {
    LogicalRect bounds;
    double corner_radius{};
    bool operator==(const SurfaceInputRegion& b) const {
        return bounds.x==b.bounds.x && bounds.y==b.bounds.y && bounds.width==b.bounds.width &&
            bounds.height==b.bounds.height && corner_radius==b.corner_radius;
    }
};
}
