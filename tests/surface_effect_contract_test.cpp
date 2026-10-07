#include "prism/contracts/surface_effect.hpp"
#include <cassert>
#include <limits>
#include <stdexcept>

namespace {
using namespace prism::contracts;

void Rejected(const SurfaceEffectRegion &region)
{
    bool rejected = false;
    try {
        ValidateSurfaceEffectRegion(region);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
}
} // namespace

int main()
{
    SurfaceEffectRegion rectangle{{-.5, .25, 96, 80}, 256, 48};
    ValidateSurfaceEffectRegion(rectangle);
    const Contour polygon{{{-.5, .25}, {95.5, .25}, {95.5, 80.25}, {47.5, 40.25}, {-.5, 80.25}}};
    SurfaceEffectRegion region{ContourBounds(polygon), 0, 12, polygon};
    ValidateSurfaceEffectRegion(region);
    assert(region != rectangle);
    auto changed = region;
    changed.contour->points[3].x += 1;
    ValidateSurfaceEffectRegion(changed);
    assert(changed.bounds == region.bounds && changed != region);

    for (double blur : {-1.0, 48.25, std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::quiet_NaN()}) {
        changed = region;
        changed.blur_radius = blur;
        Rejected(changed);
    }
    changed = region;
    changed.bounds.x += .25;
    Rejected(changed);
    changed = region;
    changed.corner_radius = 1;
    Rejected(changed);
    changed = region;
    changed.contour->points[3].x += .1;
    Rejected(changed);
    changed = region;
    changed.contour->points[3] = changed.contour->points[0];
    Rejected(changed);

    for (const auto bounds :
         {LogicalRect{0, 0, 0, 10}, {0, 0, 10, -1}, {8192.25, 0, 10, 10}, {0, 0, 8192.25, 10}}) {
        changed = rectangle;
        changed.bounds = bounds;
        Rejected(changed);
    }
    rectangle.bounds = {-8192, -8192, 8192, 8192};
    ValidateSurfaceEffectRegion(rectangle);
}
