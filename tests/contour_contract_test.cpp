#include "prism/contracts/contour.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

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

Contour Polygon(std::size_t count)
{
    Contour polygon;
    for (std::size_t index = 0; index < count; ++index) {
        const double angle = index * 2 * std::numbers::pi / count;
        polygon.points.push_back({std::round(64 * std::cos(angle) * 256) / 256,
                                  std::round(64 * std::sin(angle) * 256) / 256});
    }
    return polygon;
}

bool InMask(LogicalPoint point, const std::vector<LogicalRect> &mask)
{
    for (const auto rect : mask) {
        if (point.x >= rect.x && point.x < rect.x + rect.width && point.y >= rect.y &&
            point.y < rect.y + rect.height) {
            return true;
        }
    }
    return false;
}

void CheckMask(std::span<const Contour> contours, LogicalRect clip)
{
    const auto mask = RasterizeContourIntersection(contours, clip);
    for (int y = -12; y < 20; ++y) {
        for (int x = -12; x < 20; ++x) {
            const LogicalPoint point{x + .5, y + .5};
            bool expected = point.x >= clip.x && point.x < clip.x + clip.width &&
                            point.y >= clip.y && point.y < clip.y + clip.height;
            for (const auto &contour : contours) {
                expected = expected && ContourContains(contour, point);
            }
            assert(InMask(point, mask) == expected);
        }
    }
    for (const auto rect : mask) {
        assert(rect.width > 0 && rect.height > 0);
        assert(std::floor(rect.x) == rect.x && std::floor(rect.y) == rect.y);
        assert(std::floor(rect.width) == rect.width && std::floor(rect.height) == rect.height);
    }
}

void Geometry()
{
    const auto rect = Rectangle(-.5, .25, 12.5, 8.75);
    ValidateContour(rect);
    assert(ContourBounds(rect) == LogicalRect(-.5, .25, 12.5, 8.75));
    assert(ContourContains(rect, {2, 3}));
    assert(!ContourContains(rect, {12.25, 3}));
    assert(ContourContains(rect, {12, 3}));
    assert(!ContourContains(rect, {12, 3}, false));
    assert(ContourContains(rect, {-.5, .25}));
    assert(!ContourContains(rect, {-.5, .25}, false));
    assert(!ContourContains(rect, {std::numeric_limits<double>::quiet_NaN(), 2}));
    assert(!ContourContains(rect, {2, std::numeric_limits<double>::infinity()}));

    auto reversed = rect;
    std::reverse(reversed.points.begin(), reversed.points.end());
    ValidateContour(reversed);
    for (LogicalPoint point : {LogicalPoint{2, 3}, {12, 3}, {12.25, 3}, {-.5, .25}}) {
        assert(ContourContains(rect, point) == ContourContains(reversed, point));
    }

    const Contour concave{{{0, 0}, {8, 0}, {8, 8}, {6, 8}, {6, 2}, {2, 2}, {2, 8}, {0, 8}}};
    ValidateContour(concave);
    assert(ContourContains(concave, {1, 5}));
    assert(ContourContains(concave, {7, 5}));
    assert(!ContourContains(concave, {4, 5}));
    assert(ContourContains(concave, {2, 2}));
    assert(!ContourContains(concave, {2, 2}, false));
    assert(ContourContains(concave, {4, 1}));
    CheckMask(std::span(&concave, 1), {-4, -4, 20, 20});

    ValidateContour(Polygon(ContourVertexLimit));
    Rejected([] { ValidateContour(Polygon(ContourVertexLimit + 1)); });
    for (const auto &invalid :
         {Contour{}, Contour{{{0, 0}, {1, 1}}}, Contour{{{0, 0}, {1, 1}, {2, 2}}},
          Contour{{{0, 0}, {8, 8}, {0, 8}, {8, 0}}},
          Contour{{{0, 0}, {8, 0}, {8, 8}, {0, 8}, {8, 0}}},
          Contour{{{0, 0}, {8, 0}, {8, 0}, {8, 8}, {0, 8}}},
          Contour{{{0, 0}, {8, 0}, {4, 0}, {8, 8}, {0, 8}}},
          // A nonadjacent edge touches the initial bottom edge at (4, 0).
          Contour{{{0, 0}, {8, 0}, {8, 8}, {4, 0}, {0, 8}}}, Rectangle(.1, 0, 8, 8),
          Rectangle(8193, 0, 1, 1), Rectangle(-8192, 0, 16384, 1), Rectangle(0, -8192, 1, 16384),
          Rectangle(std::numeric_limits<double>::quiet_NaN(), 0, 8, 8),
          Rectangle(0, std::numeric_limits<double>::infinity(), 8, 8)}) {
        Rejected([&invalid] { ValidateContour(invalid); });
    }
    ValidateContour(Rectangle(-8192, -8192, 8192, 8192));
    ValidateContour(Rectangle(0, 0, 8192, 8192));
}

void Codec()
{
    const Contour triangle{{{-.5, .25}, {1.5, .25}, {.5, 1.25}}};
    const std::vector<std::uint8_t> golden{1,    0,    3,    0, 0, 0,    0,    0, 0x80, 0xff, 0xff,
                                           0xff, 0x40, 0,    0, 0, 0x80, 1,    0, 0,    0x40, 0,
                                           0,    0,    0x80, 0, 0, 0,    0x40, 1, 0,    0};
    const auto encoded = EncodeContour(triangle);
    assert(encoded == golden);
    assert(DecodeContour(golden) == triangle);
    const auto largest = EncodeContour(Polygon(ContourVertexLimit));
    assert(largest.size() == 8 + ContourVertexLimit * 8);
    assert(DecodeContour(largest) == Polygon(ContourVertexLimit));

    for (std::size_t size = 0; size < golden.size(); ++size) {
        Rejected([&golden, size] { DecodeContour(std::span(golden.data(), size)); });
    }
    auto changed = golden;
    changed.push_back(0);
    Rejected([&changed] { DecodeContour(changed); });
    for (std::size_t field : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u}) {
        changed = golden;
        changed[field] = field == 0 ? 2 : field == 2 ? 2 : 1;
        Rejected([&changed] { DecodeContour(changed); });
    }
    // Signed Q24.8 values beyond the contract bound must not wrap or clamp.
    changed = golden;
    changed[8] = 0;
    changed[9] = 0;
    changed[10] = 0x80;
    changed[11] = 0x7f;
    Rejected([&changed] { DecodeContour(changed); });
    changed = golden;
    std::copy_n(changed.begin() + 8, 8, changed.begin() + 16);
    Rejected([&changed] { DecodeContour(changed); });
    Rejected([] { EncodeContour(Rectangle(.1, 0, 8, 8)); });
}

double DistanceToSegment(LogicalPoint point, LogicalPoint a, LogicalPoint b)
{
    const double x = b.x - a.x;
    const double y = b.y - a.y;
    const double length = x * x + y * y;
    const double t =
        length ? std::clamp(((point.x - a.x) * x + (point.y - a.y) * y) / length, 0.0, 1.0) : 0;
    return std::hypot(point.x - a.x - t * x, point.y - a.y - t * y);
}

void Preparation()
{
    ContourPath rectangle{{.0001, .0001},
                          {ContourLine{{.0002, .0002}}, ContourLine{{8, 0}}, ContourLine{{8, 8}},
                           ContourLine{{0, 8}}, ContourLine{{0, 0}}}};
    const auto normalized = PrepareContour(rectangle);
    assert(normalized == Rectangle(0, 0, 8, 8));
    assert(PrepareContour(rectangle) == normalized);

    const ContourPath curve{{0, 0}, {ContourCubic{{0, 64}, {64, 64}, {64, 0}}}};
    const auto polygon = PrepareContour(curve);
    ValidateContour(polygon);
    assert(polygon.points.size() > 8 && polygon.points.size() <= ContourVertexLimit);
    assert(ContourBounds(polygon) == LogicalRect(0, 0, 64, 48));
    for (int sample = 0; sample <= 1000; ++sample) {
        const double t = sample / 1000.0;
        const double u = 1 - t;
        const LogicalPoint point{3 * u * t * t * 64 + t * t * t * 64, 3 * u * t * 64};
        double distance = std::numeric_limits<double>::infinity();
        for (std::size_t index = 1; index < polygon.points.size(); ++index) {
            distance = std::min(distance, DistanceToSegment(point, polygon.points[index - 1],
                                                            polygon.points[index]));
        }
        // Quantization can move each prepared endpoint by sqrt(2)/512.
        assert(distance <= ContourFlattenTolerance + std::sqrt(2.0) / 512);
    }

    // Flatness against an infinite supporting line alone would silently discard
    // this collinear overshoot. The real curve doubles back on itself.
    const ContourPath overshoot{
        {0, 0},
        {ContourCubic{{20, 0}, {20, 0}, {10, 0}}, ContourLine{{10, 10}}, ContourLine{{0, 10}}}};
    Rejected([&overshoot] { PrepareContour(overshoot); });
    Rejected([] { PrepareContour({{0, 0}, {}}); });
    Rejected([] { PrepareContour({{0, 0}, {ContourCubic{{8193, 0}, {0, 1}, {1, 1}}}}); });
    Rejected([] {
        PrepareContour({{4096, 0},
                        {ContourCubic{{6358.732, 0}, {8192, 1833.268}, {8192, 4096}},
                         ContourCubic{{8192, 6358.732}, {6358.732, 8192}, {4096, 8192}},
                         ContourCubic{{1833.268, 8192}, {0, 6358.732}, {0, 4096}},
                         ContourCubic{{0, 1833.268}, {1833.268, 0}, {4096, 0}}}});
    });

    ContourPath too_many;
    const auto excessive = Polygon(ContourVertexLimit + 1);
    too_many.start = excessive.points.front();
    for (std::size_t index = 1; index < excessive.points.size(); ++index) {
        too_many.segments.push_back(ContourLine{excessive.points[index]});
    }
    Rejected([&too_many] { PrepareContour(too_many); });

    const auto maximal = Polygon(ContourVertexLimit);
    ContourPath explicitly_closed;
    explicitly_closed.start = maximal.points.front();
    for (std::size_t index = 1; index < maximal.points.size(); ++index) {
        explicitly_closed.segments.push_back(ContourLine{maximal.points[index]});
    }
    explicitly_closed.segments.push_back(ContourLine{maximal.points.front()});
    assert(PrepareContour(explicitly_closed) == maximal);
    // Returning to the start before the final endpoint creates a self-touch,
    // even though preparation is allowed to remove a repeated closing point.
    const ContourPath prematurely_closed{
        {0, 0},
        {ContourLine{{8, 0}}, ContourLine{{8, 8}}, ContourLine{{0, 0}}, ContourLine{{0, 8}}}};
    Rejected([&prematurely_closed] { PrepareContour(prematurely_closed); });
}

Contour FragmentingComb()
{
    // Sixty thin teeth stay disjoint above a single shared base. Their sides
    // taper at staggered fractional phases, forcing over 65536 rectangles even
    // after equal spans on adjacent rows are coalesced. Only 244 vertices are
    // required; neither the vertex nor the coordinate budget causes rejection.
    Contour contour{{{0, 0}, {3840, 0}, {3840, 1}}};
    for (int index = 59; index >= 0; --index) {
        const double offset = index * 64 + index / 64.0;
        contour.points.push_back({offset + 56, 1});
        contour.points.push_back({offset + 33, 8192});
        contour.points.push_back({offset + 31, 8192});
        contour.points.push_back({offset + 8, 1});
    }
    contour.points.push_back({0, 1});
    return contour;
}

void Rasterization()
{
    for (double x : {-3.75, 0.0, .5, 1.25}) {
        for (double y : {-2.5, 0.0, .25}) {
            const std::array shapes{Rectangle(x, y, 12.5, 8.75),
                                    Contour{{{-3.5, -3.5}, {11.5, 2.5}, {4.5, 12.5}}}};
            CheckMask(std::span(shapes.data(), 1), {-8.25, -8.25, 24.5, 24.5});
            CheckMask(shapes, {-2.25, -1.75, 10.5, 12.5});
        }
    }
    const auto rectangle = Rectangle(.25, .25, 12, 8);
    const auto mask = RasterizeContourIntersection(std::span(&rectangle, 1), {-4, -4, 20, 20});
    assert(mask.size() == 1 && mask.front() == LogicalRect(0, 0, 12, 8));
    assert(RasterizeContourIntersection({}, {-4, -4, 20, 20}).empty());
    assert(RasterizeContourIntersection(std::span(&rectangle, 1), {0, 0, 0, 8}).empty());
    std::array<Contour, 9> excessive;
    excessive.fill(rectangle);
    Rejected([&excessive] { RasterizeContourIntersection(excessive, {0, 0, 20, 20}); });
    Rejected(
        [&rectangle] { RasterizeContourIntersection(std::span(&rectangle, 1), {0, 0, -1, 8}); });
    Rejected([&rectangle] {
        RasterizeContourIntersection(std::span(&rectangle, 1),
                                     {0, 0, std::numeric_limits<double>::infinity(), 8});
    });
    const auto invalid = Rectangle(.1, 0, 8, 8);
    Rejected([&invalid] { RasterizeContourIntersection(std::span(&invalid, 1), {0, 0, 20, 20}); });

    // Horizontal boundaries and convex/concave extrema fall exactly on pixel
    // centers. Parity alone would omit some boundary vertices or double-toggle.
    for (const auto &boundary : {Contour{{{.5, -2.5}, {5.5, 2.5}, {.5, 7.5}, {-4.5, 2.5}}},
                                 Contour{{{-5.5, -3.5},
                                          {8.5, -3.5},
                                          {8.5, 8.5},
                                          {5.5, 8.5},
                                          {2.5, 2.5},
                                          {-.5, 8.5},
                                          {-5.5, 8.5}}},
                                 Contour{{{.5, .5},
                                          {4.5, .5},
                                          {8.5, .5},
                                          {8.5, 4.5},
                                          {8.5, 8.5},
                                          {4.5, 8.5},
                                          {.5, 8.5},
                                          {.5, 4.5}}}}) {
        ValidateContour(boundary);
        CheckMask(std::span(&boundary, 1), {-10, -10, 30, 30});
        auto reversed = boundary;
        std::reverse(reversed.points.begin(), reversed.points.end());
        CheckMask(std::span(&reversed, 1), {-10, -10, 30, 30});
        const std::array intersection{boundary, Rectangle(-1.5, -1.5, 7, 7)};
        CheckMask(intersection, {-1.5, -1.5, 7, 7});
    }

    const auto complex_mask = FragmentingComb();
    ValidateContour(complex_mask);
    assert(complex_mask.points.size() == 244);
    Rejected([&complex_mask] {
        RasterizeContourIntersection(std::span(&complex_mask, 1), {0, 0, 3840, 8192});
    });
}
} // namespace

int main()
{
    Geometry();
    Codec();
    Preparation();
    Rasterization();
}
