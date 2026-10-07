#include "scene_contour_p.hpp"
#include <cmath>
#include <stdexcept>

namespace prism::runtime {

std::shared_ptr<const contracts::Contour>
PlaceSceneContour(const contracts::Contour &source, contracts::LogicalPoint origin,
                  const std::shared_ptr<const contracts::Contour> &previous)
{
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || std::abs(origin.x) > 16384 ||
        std::abs(origin.y) > 16384) {
        throw std::invalid_argument("Contour layout origin exceeds finite limits");
    }
    origin.x = std::round(origin.x * 256) / 256;
    origin.y = std::round(origin.y * 256) / 256;
    bool unchanged = previous && previous->points.size() == source.points.size();
    for (std::size_t i = 0; unchanged && i < source.points.size(); ++i) {
        unchanged = previous->points[i] == contracts::LogicalPoint{source.points[i].x + origin.x,
                                                                   source.points[i].y + origin.y};
    }
    if (unchanged) {
        return previous;
    }

    auto placed = source;
    for (auto &point : placed.points) {
        point.x += origin.x;
        point.y += origin.y;
    }
    contracts::ValidateContour(placed);
    return std::make_shared<const contracts::Contour>(std::move(placed));
}

void PrepareSceneContours(SceneSnapshot &snapshot)
{
    // Work on the detached snapshot. The caller publishes live bounds and input
    // only after every contour has passed final surface-coordinate validation.
    for (auto &node : snapshot.nodes) {
        if (!node.id || !node.style.visible || node.bounds.width <= 0 || node.bounds.height <= 0) {
            node.contour.reset();
            continue;
        }
        if (!node.contour_source) {
            if (!node.contour_spec) {
                node.contour.reset();
            }
            continue;
        }
        node.contour =
            PlaceSceneContour(*node.contour_source, {node.bounds.x, node.bounds.y}, node.contour);
    }
    PrepareAttachedPanelContours(snapshot);
}

bool SceneShapeContains(const SceneRegionShape &shape, contracts::LogicalPoint point)
{
    return shape.contour ? contracts::ContourContains(*shape.contour, point)
                         : contracts::RoundedRegionContains(point, shape.rounded);
}

} // namespace prism::runtime
