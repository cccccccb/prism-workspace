#pragma once

#include "prism/contracts/surface_effect.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace prism::contracts {
inline bool ValidRoundedRegion(SurfaceInputRegion shape) noexcept
{
    const auto &b = shape.bounds;
    return std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.width) &&
           std::isfinite(b.height) && b.width >= 0 && b.height >= 0 &&
           std::isfinite(b.x + b.width) && std::isfinite(b.y + b.height) &&
           std::isfinite(shape.corner_radius);
}

inline SurfaceInputRegion NormalizeRoundedRegion(SurfaceInputRegion shape) noexcept
{
    if (!ValidRoundedRegion(shape)) {
        return {};
    }
    shape.corner_radius =
        std::clamp(shape.corner_radius, 0.0, std::min(shape.bounds.width, shape.bounds.height) / 2);
    return shape;
}

inline bool RoundedRegionContains(LogicalPoint point, SurfaceInputRegion shape,
                                  bool include_edges = false) noexcept
{
    if (!ValidRoundedRegion(shape) || !std::isfinite(point.x) || !std::isfinite(point.y)) {
        return false;
    }
    shape = NormalizeRoundedRegion(shape);
    const auto &b = shape.bounds;
    if (b.width <= 0 || b.height <= 0 || point.x < b.x || point.y < b.y ||
        point.x > b.x + b.width || point.y > b.y + b.height ||
        (!include_edges && (point.x == b.x + b.width || point.y == b.y + b.height))) {
        return false;
    }
    const auto r = shape.corner_radius;
    const auto x = std::clamp(point.x, b.x + r, b.x + b.width - r);
    const auto y = std::clamp(point.y, b.y + r, b.y + b.height - r);
    return (point.x - x) * (point.x - x) + (point.y - y) * (point.y - y) <= r * r;
}

inline std::pair<double, double> RoundedRegionSpan(SurfaceInputRegion shape, double y) noexcept
{
    shape = NormalizeRoundedRegion(shape);
    const auto &b = shape.bounds;
    if (!std::isfinite(y) || y < b.y || y >= b.y + b.height) {
        return {0, 0};
    }
    const auto r = shape.corner_radius;
    const double edge = std::min(y - b.y, b.y + b.height - y);
    const double inset =
        edge < r ? r - std::sqrt(std::max(0.0, r * r - (r - edge) * (r - edge))) : 0;
    return {b.x + inset, b.x + b.width - inset};
}

// Integer logical pixels whose centers lie in every shape. Consecutive equal
// spans are merged; this is an input mask, not an antialiasing coverage mask.
inline std::vector<LogicalRect>
RasterizeRoundedIntersection(std::span<const SurfaceInputRegion> shapes)
{
    if (shapes.empty()) {
        return {};
    }
    double left = -std::numeric_limits<double>::infinity();
    double top = left;
    double right = -left;
    double bottom = right;
    for (const auto &shape : shapes) {
        if (!ValidRoundedRegion(shape)) {
            throw std::invalid_argument("Invalid rounded input region");
        }
        left = std::max(left, shape.bounds.x);
        top = std::max(top, shape.bounds.y);
        right = std::min(right, shape.bounds.x + shape.bounds.width);
        bottom = std::min(bottom, shape.bounds.y + shape.bounds.height);
    }
    if (right <= left || bottom <= top) {
        return {};
    }
    constexpr double limit = std::numeric_limits<int>::max() / 2;
    if (left < -limit || top < -limit || right > limit || bottom > limit || bottom - top > 65536) {
        throw std::length_error("Rounded input mask exceeds raster limits");
    }

    std::vector<LogicalRect> result;
    const int first_y = static_cast<int>(std::ceil(top - .5));
    const int end_y = static_cast<int>(std::ceil(bottom - .5));
    for (int y = first_y; y < end_y; ++y) {
        double a = left, b = right;
        for (const auto &shape : shapes) {
            const auto span = RoundedRegionSpan(shape, y + .5);
            a = std::max(a, span.first);
            b = std::min(b, span.second);
        }
        const double first = std::ceil(a - .5);
        const double end = std::min(std::floor(b - .5) + 1, std::ceil(right - .5));
        if (end <= first) {
            continue;
        }
        if (!result.empty() && result.back().x == first && result.back().width == end - first &&
            result.back().y + result.back().height == y) {
            ++result.back().height;
        } else {
            result.push_back({first, double(y), end - first, 1});
        }
    }
    return result;
}
} // namespace prism::contracts
