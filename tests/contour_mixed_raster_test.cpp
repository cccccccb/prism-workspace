#include "prism/contracts/contour.hpp"
#include "prism/contracts/rounded_region.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace prism::contracts;

template <typename Callable> void Rejected(Callable callable)
{
    bool rejected = false;
    try {
        callable();
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
}

Contour Rectangle(double x, double y, double width, double height)
{
    return {{{x, y}, {x + width, y}, {x + width, y + height}, {x, y + height}}};
}

bool InMask(LogicalPoint point, const std::vector<LogicalRect> &mask)
{
    for (const auto &rect : mask) {
        if (point.x >= rect.x && point.x < rect.x + rect.width && point.y >= rect.y &&
            point.y < rect.y + rect.height) {
            return true;
        }
    }
    return false;
}

bool Expected(LogicalPoint point, std::span<const Contour> contours, LogicalRect clip,
              std::span<const SurfaceInputRegion> rounded)
{
    if (contours.empty() && rounded.empty()) {
        return false;
    }
    if (point.x < clip.x || point.x >= clip.x + clip.width || point.y < clip.y ||
        point.y >= clip.y + clip.height) {
        return false;
    }
    for (const auto &contour : contours) {
        if (!ContourContains(contour, point)) {
            return false;
        }
    }
    for (const auto &shape : rounded) {
        if (!RoundedRegionContains(point, shape)) {
            return false;
        }
    }
    return true;
}

std::vector<LogicalRect> CheckMask(std::span<const Contour> contours, LogicalRect clip,
                                   std::span<const SurfaceInputRegion> rounded)
{
    const auto mask = RasterizeContourIntersection(contours, clip, rounded);
    assert(mask.size() <= 65536);
    for (const auto &rect : mask) {
        assert(rect.width > 0 && rect.height > 0);
        assert(std::floor(rect.x) == rect.x && std::floor(rect.y) == rect.y);
        assert(std::floor(rect.width) == rect.width && std::floor(rect.height) == rect.height);
    }

    for (int y = -24; y < 80; ++y) {
        for (int x = -24; x < 80; ++x) {
            const LogicalPoint point{x + .5, y + .5};
            assert(InMask(point, mask) == Expected(point, contours, clip, rounded));
        }
    }
    return mask;
}

void MixedConcavityAndWinding()
{
    // At y=12 this shape has two disconnected spans. Intersecting its bounds
    // with rounded clips would incorrectly retain the central notch.
    const Contour concave{{{-.5, -.5},
                           {39.5, -.5},
                           {39.5, 35.5},
                           {27.5, 35.5},
                           {27.5, 8.5},
                           {11.5, 8.5},
                           {11.5, 35.5},
                           {-.5, 35.5}}};
    const Contour neck{{{15.5, -5.5},
                        {22.5, -5.5},
                        {22.5, 2.5},
                        {34.5, 2.5},
                        {34.5, 30.5},
                        {3.5, 30.5},
                        {3.5, 2.5},
                        {15.5, 2.5}}};
    const std::array contours{concave, neck};
    const std::array rounded{SurfaceInputRegion{{-2.25, -4.75, 43.5, 44.25}, 9.5},
                             SurfaceInputRegion{{1.125, -7.375, 36.75, 43.5}, 5.25}};
    const LogicalRect clip{-4.5, -8.5, 48, 48};
    const auto mask = CheckMask(contours, clip, rounded);
    assert(InMask({7.5, 12.5}, mask));
    assert(InMask({31.5, 12.5}, mask));
    assert(!InMask({19.5, 12.5}, mask));

    auto reversed = contours;
    for (auto &contour : reversed) {
        std::reverse(contour.points.begin(), contour.points.end());
    }
    assert(CheckMask(reversed, clip, rounded) == mask);
    auto reversed_rounded = rounded;
    std::reverse(reversed_rounded.begin(), reversed_rounded.end());
    assert(CheckMask(contours, clip, reversed_rounded) == mask);
}

void FractionalBoundaries()
{
    const auto covering = Rectangle(-16.5, -16.5, 72, 72);
    const std::array shapes{SurfaceInputRegion{{.5, .5, 10, 10}, 5},
                            SurfaceInputRegion{{-3.5, -3.5, 18, 18}, 0}};
    const auto mask = CheckMask(std::span(&covering, 1), {-8.5, -8.5, 32, 32}, shapes);
    // A 3-4-5 point is exactly on the circular boundary and remains inside.
    assert(InMask({2.5, 1.5}, mask));
    assert(!InMask({1.5, 1.5}, mask));
    assert(InMask({5.5, .5}, mask));
    assert(!InMask({10.5, 5.5}, mask));
    assert(!InMask({5.5, 10.5}, mask));

    const SurfaceInputRegion square{{.5, .5, 8, 8}, 0};
    const auto square_mask =
        CheckMask(std::span(&covering, 1), {.5, .5, 4, 4}, std::span(&square, 1));
    assert(InMask({.5, .5}, square_mask));
    assert(!InMask({4.5, 2.5}, square_mask));
    assert(!InMask({2.5, 4.5}, square_mask));

    const Contour diagonal{{{-6.5, -4.5}, {21.5, 9.5}, {5.5, 32.5}}};
    for (double x : {-3.75, .0, .5, 1.125}) {
        for (double y : {-2.5, .0, .25, 1.375}) {
            for (double radius : {-2.0, .0, .25, 8.0, 40.0}) {
                const SurfaceInputRegion shape{{x, y, 24.5, 20.75}, radius};
                CheckMask(std::span(&diagonal, 1), {-8.25, -8.25, 48.5, 48.5},
                          std::span(&shape, 1));
            }
        }
    }
    // Layout clips need not be Q24.8 values; only canonical polygons are.
    const SurfaceInputRegion non_grid{{.1, -.2, 24.7, 19.3}, 6.1};
    CheckMask(std::span(&diagonal, 1), {-7.1, -5.2, 38.3, 35.7}, std::span(&non_grid, 1));
}

void EmptyAndSingleKind()
{
    const auto rectangle = Rectangle(.25, .25, 24, 16);
    const LogicalRect clip{-8, -8, 64, 64};
    assert(CheckMask(std::span(&rectangle, 1), clip, {}) ==
           RasterizeContourIntersection(std::span(&rectangle, 1), clip));

    const std::array rounded{SurfaceInputRegion{{.25, .25, 40, 32}, 8},
                             SurfaceInputRegion{{2.5, -1.25, 32.25, 40}, 12}};
    assert(CheckMask({}, clip, rounded) == RasterizeRoundedIntersection(rounded));
    assert(CheckMask({}, clip, {}).empty());
    assert(CheckMask(std::span(&rectangle, 1), {0, 0, 0, 16}, rounded).empty());
    assert(CheckMask(std::span(&rectangle, 1), {0, 0, 24, 0}, rounded).empty());

    for (const auto &shape :
         {SurfaceInputRegion{{0, 0, 0, 24}, 2}, SurfaceInputRegion{{0, 0, 24, 0}, 2},
          SurfaceInputRegion{{40, 40, 8, 8}, 2}}) {
        assert(CheckMask(std::span(&rectangle, 1), clip, std::span(&shape, 1)).empty());
    }
    // Two closed polygon edges may meet at a pixel center, but a half-open
    // rounded region touching only that right or bottom edge excludes it.
    const auto closed = Rectangle(.5, .5, 24, 16);
    const auto adjacent = Rectangle(24.5, .5, 8, 16);
    const std::array touching{closed, adjacent};
    assert(InMask({24.5, 8.5}, CheckMask(touching, clip, {})));
    const SurfaceInputRegion half_open{{.5, .5, 24, 16}, 0};
    assert(CheckMask(touching, clip, std::span(&half_open, 1)).empty());
}

void ValidationAndCount()
{
    const auto rectangle = Rectangle(0, 0, 24, 24);
    std::array<Contour, 4> contours;
    contours.fill(rectangle);
    std::array<SurfaceInputRegion, 5> rounded;
    rounded.fill({{0, 0, 24, 24}, 4});
    const auto accepted = CheckMask(contours, {0, 0, 24, 24}, std::span(rounded.data(), 4));
    assert(!accepted.empty());
    Rejected([&] { RasterizeContourIntersection(contours, {0, 0, 24, 24}, rounded); });
    std::array<SurfaceInputRegion, 8> only_rounded;
    only_rounded.fill(rounded.front());
    assert(!CheckMask({}, {0, 0, 24, 24}, only_rounded).empty());
    Rejected([&] { RasterizeContourIntersection(std::span(&rectangle, 1), {}, only_rounded); });

    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto infinity = std::numeric_limits<double>::infinity();
    for (const auto &invalid :
         {SurfaceInputRegion{{nan, 0, 8, 8}, 0}, SurfaceInputRegion{{0, nan, 8, 8}, 0},
          SurfaceInputRegion{{0, 0, infinity, 8}, 0}, SurfaceInputRegion{{0, 0, 8, infinity}, 0},
          SurfaceInputRegion{{0, 0, 8, 8}, nan}, SurfaceInputRegion{{0, 0, 8, 8}, infinity},
          SurfaceInputRegion{{0, 0, -1, 8}, 0}, SurfaceInputRegion{{0, 0, 8, -1}, 0},
          SurfaceInputRegion{{8193, 0, 8, 8}, 0}, SurfaceInputRegion{{0, -8193, 8, 8}, 0},
          SurfaceInputRegion{{0, 0, 8193, 8}, 0}, SurfaceInputRegion{{0, 0, 8, 8193}, 0}}) {
        // Empty/disjoint bounds must not skip validation or silently replace
        // malformed rounded clips with NormalizeRoundedRegion's empty value.
        Rejected([&] { RasterizeContourIntersection({}, {}, std::span(&invalid, 1)); });
        const std::array shapes{SurfaceInputRegion{{64, 64, 8, 8}, 0}, invalid};
        Rejected([&] { RasterizeContourIntersection(contours, {0, 0, 24, 24}, shapes); });
    }
    const auto invalid_contour = Rectangle(.1, 0, 8, 8);
    Rejected([&] {
        RasterizeContourIntersection(std::span(&invalid_contour, 1), {},
                                     std::span(rounded.data(), 1));
    });
    for (const auto &invalid :
         {LogicalRect{nan, 0, 8, 8}, LogicalRect{0, 0, 8, infinity}, LogicalRect{0, 0, -1, 8},
          LogicalRect{8193, 0, 8, 8}, LogicalRect{0, 0, 8193, 8}}) {
        Rejected([&] { RasterizeContourIntersection({}, invalid, {}); });
    }
    const SurfaceInputRegion limit{{0, 0, 8192, 8192}, 8192};
    assert(!RasterizeContourIntersection({}, {4090, 4090, 16, 16}, std::span(&limit, 1)).empty());
}

Contour MovingComb()
{
    // Sixteen disjoint tooth spans move exactly one logical pixel per row.
    // The last horizontal notch joins them above a one-pixel shared base.
    // This permits an exact 65536-rectangle boundary without exhausting the
    // independent vertex, coordinate, dimension or combined-shape budgets.
    Contour contour;
    for (int tooth = 0; tooth < 16; ++tooth) {
        const double left = .5 + tooth * 4;
        contour.points.push_back({left, .5});
        contour.points.push_back({left + 2, .5});
        contour.points.push_back({left + 4098, 4096.5});
        if (tooth < 15) {
            contour.points.push_back({left + 4100, 4096.5});
        }
    }
    contour.points.push_back({4158.5, 4097.5});
    contour.points.push_back({4096.5, 4097.5});
    contour.points.push_back({4096.5, 4096.5});
    ValidateContour(contour);
    return contour;
}

void RectangleBudget()
{
    const auto comb = MovingComb();
    const SurfaceInputRegion rounded{{0, 0, 8192, 8192}, 0};
    const auto mask = RasterizeContourIntersection(std::span(&comb, 1), {0, 0, 8192, 4096},
                                                   std::span(&rounded, 1));
    assert(mask.size() == 65536);
    assert(mask.front() == LogicalRect(0, 0, 3, 1));
    assert(mask.back() == LogicalRect(4155, 4095, 3, 1));

    // One further row connects the teeth into one span: rectangle 65537.
    // Fail the whole operation instead of returning a partial or bounds mask.
    Rejected([&] {
        RasterizeContourIntersection(std::span(&comb, 1), {0, 0, 8192, 4097},
                                     std::span(&rounded, 1));
    });
    const SurfaceInputRegion smaller{{0, 0, 8192, 8}, 0};
    const auto recovered = RasterizeContourIntersection(std::span(&comb, 1), {0, 0, 8192, 4097},
                                                        std::span(&smaller, 1));
    assert(recovered.size() == 128);
}
} // namespace

int main()
{
    MixedConcavityAndWinding();
    FractionalBoundaries();
    EmptyAndSingleKind();
    ValidationAndCount();
    RectangleBudget();
}
