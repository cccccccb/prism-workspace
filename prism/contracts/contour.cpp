#include "prism/contracts/contour.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace prism::contracts {
namespace {
struct FixedPoint {
    std::int64_t x;
    std::int64_t y;
};

FixedPoint Fixed(LogicalPoint point)
{
    return {static_cast<std::int64_t>(point.x * 256), static_cast<std::int64_t>(point.y * 256)};
}

std::int64_t Cross(FixedPoint a, FixedPoint b, FixedPoint c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool OnSegment(FixedPoint point, FixedPoint a, FixedPoint b)
{
    return Cross(a, b, point) == 0 && point.x >= std::min(a.x, b.x) &&
           point.x <= std::max(a.x, b.x) && point.y >= std::min(a.y, b.y) &&
           point.y <= std::max(a.y, b.y);
}

bool Intersects(FixedPoint a, FixedPoint b, FixedPoint c, FixedPoint d)
{
    const auto ac = Cross(a, b, c);
    const auto ad = Cross(a, b, d);
    const auto ca = Cross(c, d, a);
    const auto cb = Cross(c, d, b);
    if ((ac == 0 && OnSegment(c, a, b)) || (ad == 0 && OnSegment(d, a, b)) ||
        (ca == 0 && OnSegment(a, c, d)) || (cb == 0 && OnSegment(b, c, d))) {
        return true;
    }
    return ((ac < 0 && ad > 0) || (ac > 0 && ad < 0)) && ((ca < 0 && cb > 0) || (ca > 0 && cb < 0));
}
} // namespace

void ValidateContour(const Contour &contour)
{
    const auto count = contour.points.size();
    if (count < 3 || count > ContourVertexLimit) {
        throw std::invalid_argument("Contour vertex count must be 3..256");
    }
    for (auto point : contour.points) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
            std::abs(point.x) > ContourCoordinateLimit ||
            std::abs(point.y) > ContourCoordinateLimit ||
            std::round(point.x * 256) != point.x * 256 ||
            std::round(point.y * 256) != point.y * 256) {
            throw std::invalid_argument("Contour coordinates must be bounded Q24.8 values");
        }
    }
    const auto bounds = ContourBounds(contour);
    if (bounds.width > ContourCoordinateLimit || bounds.height > ContourCoordinateLimit) {
        throw std::invalid_argument("Contour dimensions exceed 8192");
    }

    std::int64_t area = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto a = Fixed(contour.points[i]);
        const auto b = Fixed(contour.points[(i + 1) % count]);
        const auto c = Fixed(contour.points[(i + 2) % count]);
        if (a.x == b.x && a.y == b.y) {
            throw std::invalid_argument("Contour contains a zero-length edge");
        }
        if (Cross(a, b, c) == 0 && (b.x - a.x) * (c.x - b.x) + (b.y - a.y) * (c.y - b.y) < 0) {
            throw std::invalid_argument("Contour has overlapping adjacent edges");
        }
        area += a.x * b.y - a.y * b.x;
        for (std::size_t j = i + 2; j < count; ++j) {
            if (i == 0 && j + 1 == count) {
                continue;
            }
            if (Intersects(a, b, Fixed(contour.points[j]),
                           Fixed(contour.points[(j + 1) % count]))) {
                throw std::invalid_argument("Contour intersects or touches itself");
            }
        }
    }
    if (area == 0) {
        throw std::invalid_argument("Contour has no enclosed area");
    }
}

LogicalRect ContourBounds(const Contour &contour)
{
    if (contour.points.empty()) {
        return {};
    }
    auto left = contour.points.front().x;
    auto right = left;
    auto top = contour.points.front().y;
    auto bottom = top;
    for (auto point : contour.points) {
        left = std::min(left, point.x);
        right = std::max(right, point.x);
        top = std::min(top, point.y);
        bottom = std::max(bottom, point.y);
    }
    return {left, top, right - left, bottom - top};
}

bool ContourContains(const Contour &contour, LogicalPoint point, bool include_edges)
{
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
        return false;
    }

    bool inside = false;
    for (std::size_t i = 0; i < contour.points.size(); ++i) {
        const auto a = contour.points[i];
        const auto b = contour.points[(i + 1) % contour.points.size()];
        const long double cross = (static_cast<long double>(b.x) - a.x) * (point.y - a.y) -
                                  (static_cast<long double>(b.y) - a.y) * (point.x - a.x);
        if (cross == 0 && point.x >= std::min(a.x, b.x) && point.x <= std::max(a.x, b.x) &&
            point.y >= std::min(a.y, b.y) && point.y <= std::max(a.y, b.y)) {
            return include_edges;
        }
        if ((a.y > point.y) != (b.y > point.y)) {
            const long double x =
                a.x + (static_cast<long double>(point.y) - a.y) * (b.x - a.x) / (b.y - a.y);
            if (point.x < x) {
                inside = !inside;
            }
        }
    }
    return inside;
}
} // namespace prism::contracts
