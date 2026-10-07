#include "prism/contracts/contour.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace prism::contracts {
namespace {
void ValidatePoint(LogicalPoint point)
{
    if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
        std::abs(point.x) > ContourCoordinateLimit || std::abs(point.y) > ContourCoordinateLimit) {
        throw std::invalid_argument("Contour path coordinate exceeds its finite bounds");
    }
}

LogicalPoint Midpoint(LogicalPoint a, LogicalPoint b)
{
    return {(a.x + b.x) / 2, (a.y + b.y) / 2};
}

double DistanceSquared(LogicalPoint point, LogicalPoint a, LogicalPoint b)
{
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double length = dx * dx + dy * dy;
    const double t =
        length == 0 ? 0
                    : std::clamp(((point.x - a.x) * dx + (point.y - a.y) * dy) / length, 0.0, 1.0);
    const double px = point.x - (a.x + t * dx);
    const double py = point.y - (a.y + t * dy);
    return px * px + py * py;
}

void Append(Contour &contour, LogicalPoint point)
{
    const LogicalPoint quantized{std::round(point.x * 256) / 256, std::round(point.y * 256) / 256};
    if (!contour.points.empty() && contour.points.back() == quantized) {
        return;
    }
    // The repeated endpoint is permitted until implicit closure is installed.
    if (contour.points.size() >= ContourVertexLimit && quantized != contour.points.front()) {
        throw std::invalid_argument("Contour preparation exceeds the vertex budget");
    }
    contour.points.push_back(quantized);
}

void Flatten(Contour &contour, LogicalPoint start, const ContourCubic &curve, unsigned depth)
{
    constexpr double tolerance_squared = ContourFlattenTolerance * ContourFlattenTolerance;
    if (DistanceSquared(curve.control1, start, curve.end) <= tolerance_squared &&
        DistanceSquared(curve.control2, start, curve.end) <= tolerance_squared) {
        Append(contour, curve.end);
        return;
    }
    if (depth == 10) {
        throw std::invalid_argument("Contour subdivision cannot meet the tolerance");
    }

    const auto ab = Midpoint(start, curve.control1);
    const auto bc = Midpoint(curve.control1, curve.control2);
    const auto cd = Midpoint(curve.control2, curve.end);
    const auto abc = Midpoint(ab, bc);
    const auto bcd = Midpoint(bc, cd);
    const auto middle = Midpoint(abc, bcd);
    Flatten(contour, start, {ab, abc, middle}, depth + 1);
    Flatten(contour, middle, {bcd, cd, curve.end}, depth + 1);
}
} // namespace

Contour PrepareContour(const ContourPath &path)
{
    if (path.segments.empty() || path.segments.size() > ContourVertexLimit) {
        throw std::invalid_argument("Contour path segment count must be 1..256");
    }
    ValidatePoint(path.start);
    for (const auto &segment : path.segments) {
        if (const auto *line = std::get_if<ContourLine>(&segment)) {
            ValidatePoint(line->end);
        } else {
            const auto &curve = std::get<ContourCubic>(segment);
            ValidatePoint(curve.control1);
            ValidatePoint(curve.control2);
            ValidatePoint(curve.end);
        }
    }

    Contour result;
    result.points.reserve(ContourVertexLimit + 1);
    Append(result, path.start);
    auto current = path.start;
    for (const auto &segment : path.segments) {
        if (const auto *line = std::get_if<ContourLine>(&segment)) {
            Append(result, line->end);
            current = line->end;
        } else {
            const auto &curve = std::get<ContourCubic>(segment);
            Flatten(result, current, curve, 0);
            current = curve.end;
        }
    }
    if (result.points.size() > 1 && result.points.back() == result.points.front()) {
        result.points.pop_back();
    }

    ValidateContour(result);
    return result;
}
} // namespace prism::contracts
