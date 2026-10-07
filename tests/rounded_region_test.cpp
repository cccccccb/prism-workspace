#include "prism/contracts/rounded_region.hpp"
#include <array>
#include <cassert>
#include <limits>
using namespace prism::contracts;

namespace {
bool InMask(LogicalPoint p, const std::vector<LogicalRect> &mask)
{
    for (auto rect : mask) {
        if (p.x >= rect.x && p.x < rect.x + rect.width && p.y >= rect.y &&
            p.y < rect.y + rect.height) {
            return true;
        }
    }
    return false;
}

void Check(std::span<const SurfaceInputRegion> shapes)
{
    auto mask = RasterizeRoundedIntersection(shapes);
    for (int y = -8; y < 72; ++y) {
        for (int x = -8; x < 72; ++x) {
            const LogicalPoint p{x + .5, y + .5};
            bool expected = true;
            for (auto shape : shapes) {
                expected = expected && RoundedRegionContains(p, shape);
            }
            assert(InMask(p, mask) == expected);
        }
    }
}
} // namespace

int main()
{
    for (double x : {-3.75, 0.0, .5, 1.25}) {
        for (double y : {-2.5, 0.0, .25}) {
            for (double radius : {0.0, 1.0, 8.0, 40.0}) {
                std::array shapes{SurfaceInputRegion{{x, y, 40.5, 32.75}, radius},
                                  SurfaceInputRegion{{2.25, -1.5, 30.25, 42.5}, 12}};
                Check(std::span(shapes.data(), 1));
                Check(shapes);
            }
        }
    }
    const SurfaceInputRegion rectangle{{.25, .25, 20, 10}, 0};
    auto mask = RasterizeRoundedIntersection(std::span(&rectangle, 1));
    assert(mask.size() == 1 && mask.front() == LogicalRect(0, 0, 20, 10));
    assert(!RoundedRegionContains({20.25, 5}, rectangle));
    assert(RoundedRegionContains({20.25, 5}, rectangle, true));
    assert(!RoundedRegionContains({0, 0}, {{0, 0, 0, 0}, 10}));
    for (auto shape :
         {SurfaceInputRegion{{0, 0, 10, 10}, std::numeric_limits<double>::quiet_NaN()},
          SurfaceInputRegion{{0, 0, -1, 10}, 0}, SurfaceInputRegion{{0, 0, 10, 70000}, 0}}) {
        bool failed = false;
        try {
            RasterizeRoundedIntersection(std::span(&shape, 1));
        } catch (const std::exception &) {
            failed = true;
        }
        assert(failed);
    }
}
