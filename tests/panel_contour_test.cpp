#include "prism/contracts/panel_contour.hpp"

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

bool HasVertex(const Contour &contour, LogicalPoint point)
{
    return std::find(contour.points.begin(), contour.points.end(), point) != contour.points.end();
}

void Canonical(const Contour &contour)
{
    ValidateContour(contour);
    assert(contour.points.size() >= 3 && contour.points.size() <= ContourVertexLimit);
    for (const auto &point : contour.points) {
        assert(std::isfinite(point.x) && std::isfinite(point.y));
        assert(std::round(point.x * 256) == point.x * 256);
        assert(std::round(point.y * 256) == point.y * 256);
    }
    assert(DecodeContour(EncodeContour(contour)) == contour);
}

void AttachmentFootprint()
{
    const PanelContourSpec rounded{16, 24, 8};
    assert(PanelNeckHeight(rounded) == 8);
    assert(PanelAttachmentCenter({120, 80}, rounded, 60, 0, 120) == 60);
    assert(PanelAttachmentCenter({120, 80}, rounded, 0, 0, 120) == 28);
    assert(PanelAttachmentCenter({120, 80}, rounded, 120, 0, 120) == 92);
    assert(PanelAttachmentCenter({120, 80}, rounded, 60, 28, 28) == 28);
    assert(PanelAttachmentCenter({120, 80}, rounded, 60, 20, 30) == 30);
    assert(!PanelAttachmentCenter({120, 80}, rounded, 60, 0, 20));
    assert(!PanelAttachmentCenter({120, 80}, rounded, 60, 100, 140));
    assert(!PanelAttachmentCenter({30, 40}, {8, 24, 8}, 15, 0, 30));

    // The full shoulder width must fit between body corners. Exact fit is
    // legal, and the center must itself intersect the visible anchor interval.
    assert(PanelAttachmentCenter({40, 40}, {8, 24, 8}, 30, 0, 40) == 20);
    assert(PanelAttachmentCenter({24, 40}, {0, 24, 8}, 30, 0, 24) == 12);
    assert(!PanelAttachmentCenter({24, 40}, {0, 24, 8}, 30, 12.25, 24));
    assert(!PanelAttachmentCenter({20, 10}, {256, 24, 8}, 10, 0, 20));
    for (const auto &disabled :
         {PanelContourSpec{16, 0, 8}, PanelContourSpec{16, 24, 0}, PanelContourSpec{16, 0, 0}}) {
        assert(PanelNeckHeight(disabled) == 0);
        assert(!PanelAttachmentCenter({120, 80}, disabled, 60, 0, 120));
    }
}

void RoundedGeometry()
{
    const PanelContourRequest request{{120, 80}, {16, 24, 8}, PanelAttachmentEdge::Top, 60};
    const auto contour = PreparePanelContour(request);
    Canonical(contour);
    assert(contour.points.size() > 20);
    assert(ContourBounds(contour) == LogicalRect(0, -8, 120, 88));
    assert(ContourContains(contour, {60, 40}));
    assert(ContourContains(contour, {16, 16}));
    assert(ContourContains(contour, {0, 40}));
    assert(!ContourContains(contour, {0, 0}));
    assert(!ContourContains(contour, {120, 0}));
    assert(!ContourContains(contour, {0, 80}));
    assert(!ContourContains(contour, {120, 80}));

    assert(ContourContains(contour, {60, -4}));
    assert(ContourContains(contour, {60, -8}));
    assert(ContourContains(contour, {56, -8}));
    assert(!ContourContains(contour, {55, -8}));
    assert(!ContourContains(contour, {60, -8.25}));
    assert(!ContourContains(contour, {44, -4}));
    assert(!ContourContains(contour, {48.25, -7.5}));
    for (double y : {-7.75, -4.0, -.25, .25, 40.0, 79.75}) {
        assert(ContourContains(contour, {60, y}));
    }
    // The visible shoulder remains inside its advertised complete footprint.
    for (double x : {47.75, 72.25}) {
        for (double y : {-7.75, -4.0, -.25}) {
            assert(!ContourContains(contour, {x, y}));
        }
    }

    auto detached = request;
    detached.edge = PanelAttachmentEdge::None;
    const auto body = PreparePanelContour(detached);
    Canonical(body);
    assert(ContourBounds(body) == LogicalRect(0, 0, 120, 80));
    assert(!ContourContains(body, {60, -4}));
    assert(ContourContains(body, {60, .25}));
    assert(!ContourContains(body, {.25, .25}));

    const auto clamped = PreparePanelContour({{20, 10}, {256, 0, 0}});
    Canonical(clamped);
    assert(ContourBounds(clamped) == LogicalRect(0, 0, 20, 10));
    assert(!ContourContains(clamped, {0, 0}) && ContourContains(clamped, {10, 5}));
}

void MirroredGeometry()
{
    for (double center : {28.0, 60.0, 92.0}) {
        PanelContourRequest request{{120, 80}, {16, 24, 8}, PanelAttachmentEdge::Top, center};
        const auto top = PreparePanelContour(request);
        request.edge = PanelAttachmentEdge::Bottom;
        const auto bottom = PreparePanelContour(request);
        Canonical(bottom);
        assert(ContourBounds(bottom) == LogicalRect(0, 0, 120, 88));
        assert(top.points.size() == bottom.points.size());
        for (std::size_t i = 0; i < top.points.size(); ++i) {
            assert(top.points[i].x == bottom.points[i].x);
            assert(std::abs(80 - top.points[i].y - bottom.points[i].y) <= 1.0 / 256);
        }
        for (int y = -12; y < 92; ++y) {
            for (int x = -4; x < 124; ++x) {
                const LogicalPoint point{x + .5, y + .5};
                assert(ContourContains(top, point) ==
                       ContourContains(bottom, {point.x, 80 - point.y}));
            }
        }
    }

    // Fractional body/neck sizes distinguish mirroring the unquantized path
    // before preparation from reflecting an already quantized top polygon.
    PanelContourRequest request{{120, 80.1}, {16, 24, 8.1}, PanelAttachmentEdge::Top, 60};
    const auto top = PreparePanelContour(request);
    request.edge = PanelAttachmentEdge::Bottom;
    const auto bottom = PreparePanelContour(request);
    Canonical(top);
    Canonical(bottom);
    assert(HasVertex(top, {52, -4.05078125}));
    assert(HasVertex(bottom, {52, 84.1484375}));
    assert(!HasVertex(bottom, {52, 84.15234375}));
    assert(ContourBounds(bottom).y == 0);
}

void SquareGeometry()
{
    PanelContourRequest request{{120.25, 80.5}, {0, 24, 8}, PanelAttachmentEdge::Top, 60.125};
    const auto top = PreparePanelContour(request);
    Canonical(top);
    assert(top.points.size() == 8);
    assert(ContourBounds(top) == LogicalRect(0, -8, 120.25, 88.5));
    for (std::size_t i = 0; i < top.points.size(); ++i) {
        const auto a = top.points[i];
        const auto b = top.points[(i + 1) % top.points.size()];
        assert(a.x == b.x || a.y == b.y);
    }
    assert(ContourContains(top, {0, 0}) && ContourContains(top, {120.25, 80.5}));
    assert(ContourContains(top, {48.125, -8}) && ContourContains(top, {72.125, -8}));
    assert(ContourContains(top, {48.25, -7.75}));
    assert(!ContourContains(top, {47.75, -7.75}));

    request.edge = PanelAttachmentEdge::Bottom;
    const auto bottom = PreparePanelContour(request);
    assert(bottom.points.size() == 8);
    assert(ContourBounds(bottom) == LogicalRect(0, 0, 120.25, 88.5));
    assert(ContourContains(bottom, {48.25, 88.25}));
    request.edge = PanelAttachmentEdge::None;
    const auto detached = PreparePanelContour(request);
    const Contour rectangle{{{0, 0}, {120.25, 0}, {120.25, 80.5}, {0, 80.5}}};
    assert(detached == rectangle);

    for (const auto &disabled : {PanelContourSpec{0, 0, 8}, PanelContourSpec{0, 24, 0}}) {
        for (auto edge :
             {PanelAttachmentEdge::None, PanelAttachmentEdge::Top, PanelAttachmentEdge::Bottom}) {
            assert(PreparePanelContour({request.body, disabled, edge, 60}) == detached);
        }
    }
    const auto exact_fit =
        PreparePanelContour({{24, 40}, {0, 24, 8}, PanelAttachmentEdge::Top, 12});
    Canonical(exact_fit);
    assert(ContourBounds(exact_fit) == LogicalRect(0, -8, 24, 48));
    assert(ContourContains(exact_fit, {.5, -7.5}));
}

bool MaskContains(const std::vector<LogicalRect> &mask, LogicalPoint point)
{
    return std::any_of(mask.begin(), mask.end(), [point](LogicalRect rect) {
        return point.x >= rect.x && point.y >= rect.y && point.x < rect.x + rect.width &&
               point.y < rect.y + rect.height;
    });
}

void RoundedTriangleGeometry()
{
    const PanelContourRequest request{
        {120, 80}, {12, 64, 20, PanelNeckShape::RoundedTriangle}, PanelAttachmentEdge::Top, 60};
    const auto contour = PreparePanelContour(request);
    Canonical(contour);
    assert(ContourBounds(contour) == LogicalRect(0, -20, 120, 100));
    assert(HasVertex(contour, {60, -20}));
    assert(std::count_if(contour.points.begin(), contour.points.end(),
                         [](LogicalPoint point) { return point.y == -20; }) == 1);
    assert(ContourContains(contour, {60, -20}));
    assert(!ContourContains(contour, {59, -20}));
    assert(!ContourContains(contour, {61, -20}));
    assert(!ContourContains(contour, {60, -20.25}));
    // Keep a margin from the canonical flattened tip, rather than assuming
    // exact analytic-cubic coverage at a point next to its final chord.
    assert(ContourContains(contour, {58, -19}));
    assert(ContourContains(contour, {62, -19}));
    assert(!ContourContains(contour, {58, -19.9}));
    assert(!ContourContains(contour, {62, -19.9}));
    assert(!ContourContains(contour, {55, -19}));
    assert(!ContourContains(contour, {65, -19}));
    assert(!ContourContains(contour, {27.75, -.25}));
    assert(!ContourContains(contour, {92.25, -.25}));
    assert(ContourContains(contour, {60, 40}));
    assert(!ContourContains(contour, {0, 0}));

    for (std::size_t i = 0; i < contour.points.size(); ++i) {
        const auto a = contour.points[i];
        const auto b = contour.points[(i + 1) % contour.points.size()];
        if (a.y < 0) {
            assert(a.x >= 28 && a.x <= 92);
            assert(a.y != b.y);
            assert(HasVertex(contour, {120 - a.x, a.y}));
            if (b.y < 0) {
                // The production-size neck must stay curved throughout: the
                // previous straight flank generated an almost 19px edge.
                assert(std::hypot(b.x - a.x, b.y - a.y) < 12);
            }
        }
    }

    auto less_rounded = request;
    less_rounded.spec.radius = .25;
    const auto small_radius = PreparePanelContour(less_rounded);
    Canonical(small_radius);
    assert(small_radius != contour);
    // At equal apex height and shoulder width, the rounded tip includes more
    // area near its top than the nearly sharp triangle.
    assert(!ContourContains(small_radius, {58, -19}));
    assert(!ContourContains(small_radius, {62, -19}));

    const std::array contours{contour};
    const auto mask = RasterizeContourIntersection(contours, {-4, -24, 128, 112});
    assert(MaskContains(mask, {59.5, -19.5}));
    assert(MaskContains(mask, {60.5, -19.5}));
    assert(!MaskContains(mask, {54.5, -19.5}));
    assert(!MaskContains(mask, {65.5, -19.5}));
    for (int y = -24; y < 88; ++y) {
        for (int x = -4; x < 124; ++x) {
            const LogicalPoint point{x + .5, y + .5};
            assert(MaskContains(mask, point) == ContourContains(contour, point));
            assert(MaskContains(mask, point) == MaskContains(mask, {120 - point.x, point.y}));
        }
    }
}

void RoundedTriangleMirroredGeometry()
{
    for (double center : {44.0, 60.0, 76.0}) {
        PanelContourRequest request{{120, 80},
                                    {12, 64, 20, PanelNeckShape::RoundedTriangle},
                                    PanelAttachmentEdge::Top,
                                    center};
        const auto top = PreparePanelContour(request);
        request.edge = PanelAttachmentEdge::Bottom;
        const auto bottom = PreparePanelContour(request);
        Canonical(bottom);
        assert(ContourBounds(bottom) == LogicalRect(0, 0, 120, 100));
        assert(top.points.size() == bottom.points.size());
        for (std::size_t i = 0; i < top.points.size(); ++i) {
            assert(top.points[i].x == bottom.points[i].x);
            assert(80 - top.points[i].y == bottom.points[i].y);
            if (top.points[i].y < 0) {
                assert(HasVertex(top, {2 * center - top.points[i].x, top.points[i].y}));
            }
        }
        for (int y = -24; y < 104; ++y) {
            for (int x = -4; x < 124; ++x) {
                const LogicalPoint point{x + .5, y + .5};
                assert(ContourContains(top, point) ==
                       ContourContains(bottom, {point.x, 80 - point.y}));
            }
        }
    }

    PanelContourRequest request{
        {120, 80.1}, {12, 64, 20.1, PanelNeckShape::RoundedTriangle}, PanelAttachmentEdge::Top, 60};
    const auto top = PreparePanelContour(request);
    request.edge = PanelAttachmentEdge::Bottom;
    const auto bottom = PreparePanelContour(request);
    Canonical(top);
    Canonical(bottom);
    assert(HasVertex(top, {60, -20.1015625}));
    assert(HasVertex(bottom, {60, 100.19921875}));
    assert(!HasVertex(bottom, {60, 100.203125}));
}

void SquareTriangleGeometry()
{
    PanelContourRequest request{{120.25, 80.5},
                                {0, 24, 8, PanelNeckShape::RoundedTriangle},
                                PanelAttachmentEdge::Top,
                                60.125};
    const auto top = PreparePanelContour(request);
    Canonical(top);
    assert(top.points.size() == 7);
    assert(ContourBounds(top) == LogicalRect(0, -8, 120.25, 88.5));
    assert(HasVertex(top, {48.125, 0}));
    assert(HasVertex(top, {60.125, -8}));
    assert(HasVertex(top, {72.125, 0}));
    assert(!HasVertex(top, {48.125, -8}));
    assert(!HasVertex(top, {72.125, -8}));
    assert(ContourContains(top, {60.125, -8}));
    assert(!ContourContains(top, {59.125, -8}));
    assert(!ContourContains(top, {48.25, -7.75}));
    assert(ContourContains(top, {0, 0}));
    assert(ContourContains(top, {120.25, 80.5}));
    std::size_t diagonal_count = 0;
    for (std::size_t i = 0; i < top.points.size(); ++i) {
        const auto a = top.points[i];
        const auto b = top.points[(i + 1) % top.points.size()];
        if (a.x != b.x && a.y != b.y) {
            ++diagonal_count;
            assert(a.y <= 0 && b.y <= 0);
        }
    }
    assert(diagonal_count == 2);

    request.edge = PanelAttachmentEdge::Bottom;
    const auto bottom = PreparePanelContour(request);
    Canonical(bottom);
    assert(bottom.points.size() == 7);
    assert(ContourBounds(bottom) == LogicalRect(0, 0, 120.25, 88.5));
    assert(HasVertex(bottom, {60.125, 88.5}));
    assert(ContourContains(bottom, {60.125, 88.5}));
    assert(!ContourContains(bottom, {59.125, 88.5}));

    const auto exact_fit = PreparePanelContour(
        {{24, 40}, {0, 24, 8, PanelNeckShape::RoundedTriangle}, PanelAttachmentEdge::Top, 12});
    Canonical(exact_fit);
    assert(ContourBounds(exact_fit) == LogicalRect(0, -8, 24, 48));
    assert(ContourContains(exact_fit, {12, -7.5}));
    assert(!ContourContains(exact_fit, {.5, -7.5}));
}

void ShapeCompatibility()
{
    for (double radius : {0.0, .25, 16.0}) {
        for (auto edge :
             {PanelAttachmentEdge::None, PanelAttachmentEdge::Top, PanelAttachmentEdge::Bottom}) {
            PanelContourRequest request{{120, 80}, {radius, 28, 8}, edge, 60};
            const auto legacy = PreparePanelContour(request);
            assert(request.spec.neck_shape == PanelNeckShape::SoftTab);
            request.spec.neck_shape = PanelNeckShape::SoftTab;
            assert(legacy == PreparePanelContour(request));
            request.spec.neck_shape = PanelNeckShape::RoundedTriangle;
            const auto triangle = PreparePanelContour(request);
            if (edge == PanelAttachmentEdge::None) {
                assert(legacy == triangle);
            } else {
                assert(legacy != triangle);
            }
        }

        for (const auto &disabled :
             {PanelContourSpec{radius, 0, 8}, PanelContourSpec{radius, 28, 0},
              PanelContourSpec{radius, 0, 0}}) {
            PanelContourRequest request{{120, 80}, disabled, PanelAttachmentEdge::Top, 60};
            const auto legacy = PreparePanelContour(request);
            request.spec.neck_shape = PanelNeckShape::RoundedTriangle;
            assert(PanelNeckHeight(request.spec) == 0);
            assert(!PanelAttachmentCenter(request.body, request.spec, 60, 0, 120));
            for (auto edge : {PanelAttachmentEdge::None, PanelAttachmentEdge::Top,
                              PanelAttachmentEdge::Bottom}) {
                request.edge = edge;
                assert(legacy == PreparePanelContour(request));
            }
        }
    }

    const PanelContourSpec triangle{16, 28, 8, PanelNeckShape::RoundedTriangle};
    assert(PanelAttachmentCenter({120, 80}, triangle, 60, 0, 120) == 60);
    assert(PanelAttachmentCenter({120, 80}, triangle, 0, 0, 120) == 30);
    assert(PanelAttachmentCenter({120, 80}, triangle, 120, 0, 120) == 90);
    assert(!PanelAttachmentCenter({30, 40}, triangle, 15, 0, 30));
}

void InvalidInputs()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    for (const auto &spec :
         {PanelContourSpec{nan, 24, 8}, PanelContourSpec{16, nan, 8}, PanelContourSpec{16, 24, nan},
          PanelContourSpec{infinity, 24, 8}, PanelContourSpec{16, infinity, 8},
          PanelContourSpec{16, 24, infinity}, PanelContourSpec{-1, 24, 8},
          PanelContourSpec{16, -1, 8}, PanelContourSpec{16, 24, -1}, PanelContourSpec{257, 24, 8},
          PanelContourSpec{16, 257, 8}, PanelContourSpec{16, 24, 49},
          PanelContourSpec{16, 24, 8, PanelNeckShape(77)},
          PanelContourSpec{16, 0, 0, PanelNeckShape(77)}}) {
        Rejected([&] { ValidatePanelContourSpec(spec); });
        Rejected([&] { PanelNeckHeight(spec); });
        Rejected([&] { PanelAttachmentCenter({120, 80}, spec, 60, 0, 120); });
        Rejected([&] { PreparePanelContour({{120, 80}, spec}); });
    }
    for (const auto &body :
         {LogicalSize{0, 80}, LogicalSize{120, 0}, LogicalSize{-1, 80}, LogicalSize{120, -1},
          LogicalSize{nan, 80}, LogicalSize{120, nan}, LogicalSize{infinity, 80},
          LogicalSize{120, infinity}, LogicalSize{8193, 80}, LogicalSize{120, 8193}}) {
        Rejected([&] { PanelAttachmentCenter(body, {16, 24, 8}, 60, 0, 120); });
        Rejected([&] { PreparePanelContour({body, {16, 24, 8}}); });
    }
    for (double value : {nan, infinity, -infinity}) {
        Rejected([&] { PanelAttachmentCenter({120, 80}, {16, 24, 8}, value, 0, 120); });
        Rejected([&] { PanelAttachmentCenter({120, 80}, {16, 24, 8}, 60, value, 120); });
        Rejected([&] { PanelAttachmentCenter({120, 80}, {16, 24, 8}, 60, 0, value); });
        Rejected([&] {
            PreparePanelContour({{120, 80}, {16, 24, 8}, PanelAttachmentEdge::None, value});
        });
    }
    Rejected([] { PanelAttachmentCenter({120, 80}, {16, 24, 8}, 60, 100, 20); });
    Rejected([] { PreparePanelContour({{120, 80}, {16, 24, 8}, PanelAttachmentEdge(77), 60}); });
    for (double center : {27.75, 92.25}) {
        Rejected([&] {
            PreparePanelContour({{120, 80}, {16, 24, 8}, PanelAttachmentEdge::Top, center});
        });
    }
    Rejected([] { PreparePanelContour({{30, 40}, {8, 24, 8}, PanelAttachmentEdge::Top, 15}); });
    Rejected([] { PreparePanelContour({{.001, 40}, {}, PanelAttachmentEdge::None}); });
    Rejected(
        [] { PreparePanelContour({{120, 80}, {16, 24, .001}, PanelAttachmentEdge::Top, 60}); });
    Rejected([] { PreparePanelContour({{120, 80}, {0, .001, 8}, PanelAttachmentEdge::Top, 60}); });
    for (auto edge : {PanelAttachmentEdge::Top, PanelAttachmentEdge::Bottom}) {
        Rejected([&] { PreparePanelContour({{8192, 8192}, {256, 256, 48}, edge, 4096}); });
        Rejected([&] {
            PreparePanelContour(
                {{8192, 8192}, {256, 256, 48, PanelNeckShape::RoundedTriangle}, edge, 4096});
        });
        Rejected([&] {
            PreparePanelContour(
                {{120, 80}, {16, 24, .001, PanelNeckShape::RoundedTriangle}, edge, 60});
        });
        Rejected([&] {
            PreparePanelContour(
                {{120, 80}, {0, .001, 8, PanelNeckShape::RoundedTriangle}, edge, 60});
        });
        // Keep an explicit single-apex contract after quantization: a very
        // small rounding radius must not silently become a flat tip.
        Rejected([&] {
            PreparePanelContour(
                {{768, 640}, {.001, 256, .25, PanelNeckShape::RoundedTriangle}, edge, 384});
        });
    }
}

void BudgetAndFiniteBoundaries()
{
    ValidatePanelContourSpec({256, 256, 48});
    for (auto edge : {PanelAttachmentEdge::Top, PanelAttachmentEdge::Bottom}) {
        const auto contour = PreparePanelContour({{8192, 8144}, {256, 256, 48}, edge, 4096});
        Canonical(contour);
        assert(contour.points.size() > 64 && contour.points.size() <= 256);
        const auto bounds = ContourBounds(contour);
        assert(bounds.width == 8192 && bounds.height == 8192);
        assert(bounds.y == (edge == PanelAttachmentEdge::Top ? -48 : 0));
    }
    const auto detached = PreparePanelContour({{8192, 8192}, {256, 256, 48}});
    Canonical(detached);
    assert(ContourBounds(detached) == LogicalRect(0, 0, 8192, 8192));

    for (double radius : {0.0, .25, 2.0, 16.0, 128.0, 256.0}) {
        for (double width : {1.0, 8.0, 24.0, 128.0, 256.0}) {
            for (double height : {.25, 1.0, 8.0, 48.0}) {
                const PanelContourSpec spec{radius, width, height};
                const auto center =
                    PanelAttachmentCenter({768.25, 640.5}, spec, 384.125, 0, 768.25);
                assert(center);
                for (auto edge : {PanelAttachmentEdge::Top, PanelAttachmentEdge::Bottom}) {
                    Canonical(PreparePanelContour({{768.25, 640.5}, spec, edge, *center}));
                }
            }
        }
    }
}

void TriangleBudgetAndFiniteBoundaries()
{
    ValidatePanelContourSpec({256, 256, 48, PanelNeckShape::RoundedTriangle});
    for (auto edge : {PanelAttachmentEdge::Top, PanelAttachmentEdge::Bottom}) {
        const auto contour = PreparePanelContour(
            {{8192, 8144}, {256, 256, 48, PanelNeckShape::RoundedTriangle}, edge, 4096});
        Canonical(contour);
        const auto bounds = ContourBounds(contour);
        assert(bounds.width == 8192 && bounds.height == 8192);
        assert(bounds.y == (edge == PanelAttachmentEdge::Top ? -48 : 0));
    }

    for (double radius : {0.0, .25, 2.0, 12.0, 16.0, 128.0, 256.0}) {
        for (double width : {1.0, 8.0, 24.0, 64.0, 128.0, 256.0}) {
            for (double height : {.25, 1.0, 8.0, 20.0, 48.0}) {
                const PanelContourSpec spec{radius, width, height, PanelNeckShape::RoundedTriangle};
                const auto center =
                    PanelAttachmentCenter({768.25, 640.5}, spec, 384.125, 0, 768.25);
                assert(center);
                for (auto edge : {PanelAttachmentEdge::Top, PanelAttachmentEdge::Bottom}) {
                    const auto contour =
                        PreparePanelContour({{768.25, 640.5}, spec, edge, *center});
                    Canonical(contour);
                    const double apex_y =
                        edge == PanelAttachmentEdge::Top ? -height : 640.5 + height;
                    assert(HasVertex(contour, {*center, apex_y}));
                    assert(std::count_if(
                               contour.points.begin(), contour.points.end(),
                               [apex_y](LogicalPoint point) { return point.y == apex_y; }) == 1);
                }
            }
        }
    }
}
} // namespace

int main()
{
    AttachmentFootprint();
    RoundedGeometry();
    MirroredGeometry();
    SquareGeometry();
    RoundedTriangleGeometry();
    RoundedTriangleMirroredGeometry();
    SquareTriangleGeometry();
    ShapeCompatibility();
    InvalidInputs();
    BudgetAndFiniteBoundaries();
    TriangleBudgetAndFiniteBoundaries();
}
