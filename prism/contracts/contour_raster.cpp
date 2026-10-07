#include "prism/contracts/contour.hpp"
#include "prism/contracts/rounded_region.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace prism::contracts {
namespace {
using Span = std::pair<int, int>; // Inclusive start, exclusive end pixel indices.

void AddSpan(std::vector<Span> &spans, long double left, long double right, Span clip)
{
    const int start = std::max(clip.first, static_cast<int>(std::ceil(left - 0.5L)));
    const int end = std::min(clip.second, static_cast<int>(std::floor(right - 0.5L)) + 1);
    if (start < end) {
        spans.emplace_back(start, end);
    }
}

std::vector<Span> RowSpans(const Contour &contour, double y, Span clip)
{
    std::vector<long double> crossings;
    std::vector<Span> spans;
    crossings.reserve(contour.points.size());
    for (std::size_t i = 0; i < contour.points.size(); ++i) {
        const auto a = contour.points[i];
        const auto b = contour.points[(i + 1) % contour.points.size()];
        if (a.y == b.y) {
            if (y == a.y) {
                AddSpan(spans, std::min(a.x, b.x), std::max(a.x, b.x), clip);
            }
            continue;
        }
        if (y >= std::min(a.y, b.y) && y <= std::max(a.y, b.y)) {
            const long double x =
                a.x + (static_cast<long double>(y) - a.y) * (b.x - a.x) / (b.y - a.y);
            AddSpan(spans, x, x, clip);
            if ((a.y > y) != (b.y > y)) {
                crossings.push_back(x);
            }
        }
    }

    std::sort(crossings.begin(), crossings.end());
    for (std::size_t i = 0; i + 1 < crossings.size(); i += 2) {
        AddSpan(spans, crossings[i], crossings[i + 1], clip);
    }
    std::sort(spans.begin(), spans.end());
    std::vector<Span> merged;
    for (auto span : spans) {
        if (!merged.empty() && span.first <= merged.back().second) {
            merged.back().second = std::max(merged.back().second, span.second);
        } else {
            merged.push_back(span);
        }
    }
    return merged;
}

std::vector<Span> Intersect(const std::vector<Span> &a, const std::vector<Span> &b)
{
    std::vector<Span> result;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        const auto start = std::max(a[i].first, b[j].first);
        const auto end = std::min(a[i].second, b[j].second);
        if (start < end) {
            result.emplace_back(start, end);
        }
        if (a[i].second < b[j].second) {
            ++i;
        } else {
            ++j;
        }
    }
    return result;
}

std::vector<Span> RoundedRowSpan(const SurfaceInputRegion &shape, double y, Span clip)
{
    const auto &bounds = shape.bounds;
    if (bounds.width == 0 || bounds.height == 0 || y < bounds.y || y >= bounds.y + bounds.height) {
        return {};
    }

    const auto [left, right] = RoundedRegionSpan(shape, y);
    // Rounded input bounds are half-open on the right and bottom, while the
    // circular arc itself includes its boundary. Do not close the right edge
    // merely because AddSpan includes contour edges.
    clip.second = std::min(clip.second, static_cast<int>(std::ceil(bounds.x + bounds.width - .5)));
    std::vector<Span> result;
    AddSpan(result, left, right, clip);
    return result;
}

void ValidateClip(LogicalRect clip)
{
    if (!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.width) ||
        !std::isfinite(clip.height) || std::abs(clip.x) > ContourCoordinateLimit ||
        std::abs(clip.y) > ContourCoordinateLimit || clip.width < 0 || clip.height < 0 ||
        clip.width > ContourCoordinateLimit || clip.height > ContourCoordinateLimit) {
        throw std::invalid_argument("Contour raster clip exceeds its finite bounds");
    }
}

void ValidateRounded(const SurfaceInputRegion &shape)
{
    const auto &bounds = shape.bounds;
    if (!ValidRoundedRegion(shape) || std::abs(bounds.x) > ContourCoordinateLimit ||
        std::abs(bounds.y) > ContourCoordinateLimit || bounds.width > ContourCoordinateLimit ||
        bounds.height > ContourCoordinateLimit) {
        throw std::invalid_argument("Rounded contour raster shape exceeds its finite bounds");
    }
}
} // namespace

std::vector<LogicalRect> RasterizeContourIntersection(std::span<const Contour> contours,
                                                      LogicalRect clip)
{
    return RasterizeContourIntersection(contours, clip, {});
}

std::vector<LogicalRect> RasterizeContourIntersection(std::span<const Contour> contours,
                                                      LogicalRect clip,
                                                      std::span<const SurfaceInputRegion> rounded)
{
    ValidateClip(clip);
    if (contours.size() > 8 || rounded.size() > 8 - contours.size()) {
        throw std::invalid_argument("Contour raster combined shape limit is 8");
    }
    for (const auto &contour : contours) {
        ValidateContour(contour);
    }
    std::vector<SurfaceInputRegion> normalized;
    normalized.reserve(rounded.size());
    for (const auto &shape : rounded) {
        ValidateRounded(shape);
        normalized.push_back(NormalizeRoundedRegion(shape));
    }
    if ((contours.empty() && rounded.empty()) || clip.width == 0 || clip.height == 0) {
        return {};
    }

    double top = clip.y;
    double bottom = clip.y + clip.height;
    double left = clip.x;
    double right = clip.x + clip.width;
    for (const auto &contour : contours) {
        const auto bounds = ContourBounds(contour);
        top = std::max(top, bounds.y);
        bottom = std::min(bottom, bounds.y + bounds.height);
        left = std::max(left, bounds.x);
        right = std::min(right, bounds.x + bounds.width);
    }
    for (const auto &shape : normalized) {
        const auto &bounds = shape.bounds;
        if (bounds.width == 0 || bounds.height == 0) {
            return {};
        }
        top = std::max(top, bounds.y);
        bottom = std::min(bottom, bounds.y + bounds.height);
        left = std::max(left, bounds.x);
        right = std::min(right, bounds.x + bounds.width);
    }
    if (top > bottom || left > right) {
        return {};
    }
    // The clip is half-open, while polygon edges participate in the binary mask.
    const Span xclip{
        std::max(static_cast<int>(std::ceil(clip.x - .5)), static_cast<int>(std::ceil(left - .5))),
        std::min(static_cast<int>(std::ceil(clip.x + clip.width - .5)),
                 static_cast<int>(std::floor(right - .5)) + 1)};
    const int first = static_cast<int>(std::ceil(top - .5));
    const int last = std::min(static_cast<int>(std::ceil(clip.y + clip.height - .5)),
                              static_cast<int>(std::floor(bottom - .5)) + 1);
    std::vector<LogicalRect> rectangles;
    std::vector<Span> previous;
    std::size_t previous_start = 0;
    for (int y = first; y < last; ++y) {
        std::vector<Span> spans;
        if (xclip.first < xclip.second) {
            spans.push_back(xclip);
        }
        for (const auto &contour : contours) {
            if (spans.empty()) {
                break;
            }
            spans = Intersect(spans, RowSpans(contour, y + .5, xclip));
        }
        for (const auto &shape : normalized) {
            if (spans.empty()) {
                break;
            }
            spans = Intersect(spans, RoundedRowSpan(shape, y + .5, xclip));
        }
        if (spans == previous) {
            for (std::size_t i = previous_start; i < rectangles.size(); ++i) {
                rectangles[i].height += 1;
            }
            continue;
        }

        if (rectangles.size() + spans.size() > 65536) {
            throw std::invalid_argument("Contour raster mask exceeds 65536 rectangles");
        }
        previous_start = rectangles.size();
        for (auto [start, end] : spans) {
            rectangles.push_back({static_cast<double>(start), static_cast<double>(y),
                                  static_cast<double>(end - start), 1});
        }
        previous = std::move(spans);
    }
    return rectangles;
}
} // namespace prism::contracts
