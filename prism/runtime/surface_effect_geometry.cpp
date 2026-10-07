#include "surface_effect_geometry_p.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace prism::runtime {
namespace {
using RoundedShape = contracts::SurfaceInputRegion;
using Shape = SceneRegionShape;
using Rect = contracts::LogicalRect;

bool ContainsRoundedBox(RoundedShape outer, Rect box)
{
    return contracts::RoundedRegionContains({box.x, box.y}, outer, true) &&
           contracts::RoundedRegionContains({box.x + box.width, box.y}, outer, true) &&
           contracts::RoundedRegionContains({box.x, box.y + box.height}, outer, true) &&
           contracts::RoundedRegionContains({box.x + box.width, box.y + box.height}, outer, true);
}

bool EdgeBoundsTouchBox(contracts::LogicalPoint a, contracts::LogicalPoint b, Rect box)
{
    return std::max(a.x, b.x) >= box.x && std::min(a.x, b.x) <= box.x + box.width &&
           std::max(a.y, b.y) >= box.y && std::min(a.y, b.y) <= box.y + box.height;
}

bool ContainsContourBox(const contracts::Contour &outer, Rect box)
{
    if (!contracts::ContourContains(outer, {box.x, box.y}) ||
        !contracts::ContourContains(outer, {box.x + box.width, box.y}) ||
        !contracts::ContourContains(outer, {box.x, box.y + box.height}) ||
        !contracts::ContourContains(outer, {box.x + box.width, box.y + box.height})) {
        return false;
    }

    // Corners alone do not prove containment in a concave polygon. Reject
    // every boundary that may enter or touch the inner box; a conservative
    // edge-box test can reject a valid case, but cannot enlarge the effect.
    for (std::size_t i = 0; i < outer.points.size(); ++i) {
        if (EdgeBoundsTouchBox(outer.points[i], outer.points[(i + 1) % outer.points.size()], box)) {
            return false;
        }
    }
    return true;
}

bool ContainsShapeBox(const Shape &outer, Rect box)
{
    return outer.contour ? ContainsContourBox(*outer.contour, box)
                         : ContainsRoundedBox(outer.rounded, box);
}

std::optional<Shape> IntersectRoundedEffects(RoundedShape a, RoundedShape b, Rect box)
{
    if (a.bounds == b.bounds) {
        return Shape{{box, std::max(a.corner_radius, b.corner_radius)}, {}};
    }
    if (box == a.bounds && ContainsRoundedBox(b, a.bounds)) {
        return Shape{a, {}};
    }
    if (box == b.bounds && ContainsRoundedBox(a, b.bounds)) {
        return Shape{b, {}};
    }
    if (ContainsRoundedBox(a, box) && ContainsRoundedBox(b, box)) {
        return Shape{{box, 0}, {}};
    }

    // One uniform-radius rounded rectangle cannot express cropped curved
    // corners or an intersection of offset curved clips faithfully.
    throw std::runtime_error(
        "Unsupported backdrop clipping: intersection is not a rounded rectangle");
}

} // namespace

Rect IntersectSurfaceRegionBounds(Rect a, Rect b)
{
    const auto x = std::max(a.x, b.x);
    const auto y = std::max(a.y, b.y);
    return {x, y, std::max(0.0, std::min(a.x + a.width, b.x + b.width) - x),
            std::max(0.0, std::min(a.y + a.height, b.y + b.height) - y)};
}

bool SameSurfaceRegionShape(const Shape &a, const Shape &b)
{
    if (a.contour || b.contour) {
        return a.contour && b.contour && (a.contour == b.contour || *a.contour == *b.contour);
    }
    return a.rounded == b.rounded;
}

std::optional<Shape> IntersectSurfaceEffectShapes(const Shape &a, const Shape &b)
{
    const auto box = IntersectSurfaceRegionBounds(a.Bounds(), b.Bounds());
    if (box.width == 0 || box.height == 0) {
        return std::nullopt;
    }
    if (!a.contour && !b.contour) {
        return IntersectRoundedEffects(a.rounded, b.rounded, box);
    }
    if (SameSurfaceRegionShape(a, b)) {
        return a;
    }
    if (ContainsShapeBox(b, a.Bounds())) {
        return a;
    }
    if (ContainsShapeBox(a, b.Bounds())) {
        return b;
    }

    // The contour protocol carries one existing simple polygon. Partial or
    // ambiguous intersections may require curves or disconnected components;
    // never replace them with their bounding rectangle or a binary input mask.
    throw std::runtime_error(
        "Unsupported backdrop clipping: contour intersection is not an intact shape");
}

void ValidatePreparedSurfaceEffect(const contracts::SurfaceEffectRegion &effect)
{
    try {
        contracts::ValidateSurfaceEffectRegion(effect);
    } catch (const std::invalid_argument &error) {
        // Keep the Scene diagnostic/error boundary stable while the transport
        // contract remains the single source of geometry and numeric validation.
        const std::string prefix =
            effect.contour ? "Unsupported backdrop contour: " : "Unsupported backdrop bounds: ";
        throw std::runtime_error(prefix + error.what());
    }
}

} // namespace prism::runtime
