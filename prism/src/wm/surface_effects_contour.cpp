#include "surface_effects_contour_p.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace prism::wm::effect_detail {
namespace {
struct ScanEdge {
    double low_y{}, high_y{}, low_x{}, slope{};
};

std::vector<ScanEdge> PrepareEdges(const contracts::Contour &contour, double origin_x,
                                   double origin_y)
{
    const auto bounds = contracts::ContourBounds(contour);
    if (!std::isfinite(bounds.x) || !std::isfinite(bounds.y) || !std::isfinite(bounds.width) ||
        !std::isfinite(bounds.height) || bounds.width <= 0 || bounds.height <= 0 ||
        bounds.width > 8192 || bounds.height > 8192) {
        throw std::invalid_argument("Contour coverage bounds exceed resource limits");
    }

    std::vector<ScanEdge> edges;
    edges.reserve(contour.points.size());
    for (std::size_t index = 0; index < contour.points.size(); ++index) {
        auto a = contour.points[index];
        auto b = contour.points[(index + 1) % contour.points.size()];
        if (a.y == b.y) {
            continue;
        }
        if (a.y > b.y) {
            std::swap(a, b);
        }
        edges.push_back({a.y - bounds.y + origin_y, b.y - bounds.y + origin_y,
                         a.x - bounds.x + origin_x, (b.x - a.x) / (b.y - a.y)});
        if (!std::isfinite(edges.back().slope)) {
            throw std::invalid_argument("Contour coverage edge slope is not finite");
        }
    }
    return edges;
}

void AddInterval(std::vector<double> &coverage, double left, double right, int width)
{
    left = std::clamp(left, 0.0, double(width));
    right = std::clamp(right, 0.0, double(width));
    if (right <= left) {
        return;
    }

    const int begin = int(std::floor(left));
    const int end = int(std::ceil(right));
    for (int x = begin; x < end; ++x) {
        coverage[x] += std::min(right, double(x + 1)) - std::max(left, double(x));
    }
}

void SampleRow(const std::vector<ScanEdge> &edges, double y, std::vector<double> &intersections,
               std::vector<double> &coverage, int width)
{
    intersections.clear();
    for (const auto &edge : edges) {
        if (y >= edge.low_y && y < edge.high_y) {
            intersections.push_back(edge.low_x + (y - edge.low_y) * edge.slope);
        }
    }
    if (intersections.size() % 2 != 0) {
        throw std::invalid_argument("Contour coverage has an unmatched scanline edge");
    }

    std::sort(intersections.begin(), intersections.end());
    for (std::size_t index = 0; index < intersections.size(); index += 2) {
        AddInterval(coverage, intersections[index], intersections[index + 1], width);
    }
}
} // namespace

std::vector<std::uint8_t> RasterizeContourCoverage(const contracts::Contour &contour, int width,
                                                   int height, double origin_x, double origin_y)
{
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192 ||
        std::size_t(width) * std::size_t(height) > ContourMaskPixelLimit ||
        contour.points.size() < 3 || contour.points.size() > contracts::ContourVertexLimit ||
        !std::isfinite(origin_x) || !std::isfinite(origin_y) || std::abs(origin_x) > 8192 ||
        std::abs(origin_y) > 8192) {
        throw std::invalid_argument("Contour coverage geometry exceeds resource limits");
    }
    for (const auto &point : contour.points) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || std::abs(point.x) > 1e8 ||
            std::abs(point.y) > 1e8) {
            throw std::invalid_argument("Contour coverage point is not finite or bounded");
        }
    }

    const auto edges = PrepareEdges(contour, origin_x, origin_y);
    std::vector<std::uint8_t> pixels(std::size_t(width) * std::size_t(height));
    std::vector<double> coverage(width);
    std::vector<double> intersections;
    intersections.reserve(contour.points.size());
    constexpr int subrows = 4;

    for (int y = 0; y < height; ++y) {
        std::fill(coverage.begin(), coverage.end(), 0);
        for (int sample = 0; sample < subrows; ++sample) {
            SampleRow(edges, y + (sample + 0.5) / subrows, intersections, coverage, width);
        }
        for (int x = 0; x < width; ++x) {
            const auto alpha = std::clamp(coverage[x] / subrows, 0.0, 1.0);
            pixels[std::size_t(y) * width + x] = std::uint8_t(std::lround(alpha * 255));
        }
    }
    return pixels;
}

} // namespace prism::wm::effect_detail
