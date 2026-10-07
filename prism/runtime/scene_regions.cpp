#include "scene_contour_p.hpp"
#include "scene_p.hpp"
#include "surface_effect_geometry_p.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace prism::runtime {
namespace {
using RoundedShape = contracts::SurfaceInputRegion;
using Shape = SceneRegionShape;
using Rect = contracts::LogicalRect;

void AppendInputRegion(RoundedShape shape, std::vector<RoundedShape> &output)
{
    if (output.size() == 65536) {
        throw std::length_error("Surface input region limit is 65536 rectangles");
    }
    output.push_back(shape);
}

void AppendInputMask(const std::vector<Rect> &mask, std::vector<RoundedShape> &output)
{
    if (mask.size() > 65536 - output.size()) {
        throw std::length_error("Surface input region limit is 65536 rectangles");
    }
    for (const auto &rect : mask) {
        output.push_back({rect, 0});
    }
}

void AppendUniqueShape(const Shape &shape, std::vector<Shape> &shapes)
{
    for (const auto &existing : shapes) {
        if (SameSurfaceRegionShape(shape, existing)) {
            return;
        }
    }
    shapes.push_back(shape);
}
} // namespace

SceneRegionShape Scene::RegionShape(const Node &node, const SceneRegionPlacement *placement) const
{
    if ((node.contour_source || node.contour_recipe) && !node.contour) {
        throw std::logic_error("Contour regions require prepared visible geometry");
    }
    Shape result{contracts::NormalizeRoundedRegion({node.bounds, node.style.radius}), node.contour};
    if (!placement) {
        return result;
    }

    for (const Node *current = &node; current && current->parent; current = current->parent) {
        if (current->parent->id != placement->scroll_root) {
            continue;
        }
        if (current->kind != Kind::Visual) {
            result.rounded.bounds.y += placement->delta;
            if (node.contour_source) {
                result.contour = placement->contours.at(node.id.index);
            }
        }
        break;
    }
    return result;
}

void Scene::CollectSurfaceEffects(const Node &node, std::vector<Shape> &clips,
                                  std::vector<contracts::SurfaceEffectRegion> &result,
                                  const SceneRegionPlacement *placement) const
{
    if (node.kind == Kind::Visual || !IsVisible(node) || node.bounds.width <= 0 ||
        node.bounds.height <= 0 || (HasPopupSurfaceAdoption() && node.id == active_popup_) ||
        (placement && placement->close_popup && IsPopupKind(node.kind))) {
        return;
    }
    const bool clipped = IsPopupKind(node.kind) || node.kind == Kind::ScrollView ||
                         node.style.clip || node.style.overflow == "clip";
    if (clipped) {
        clips.push_back(RegionShape(node, placement));
    }
    if (node.style.backdrop_blur > 0) {
        std::optional<Shape> shape = RegionShape(node, placement);
        for (const auto &clip : clips) {
            shape = IntersectSurfaceEffectShapes(*shape, clip);
            if (!shape) {
                break;
            }
        }
        if (shape) {
            contracts::SurfaceEffectRegion effect{shape->Bounds(),
                                                  shape->contour ? 0 : shape->rounded.corner_radius,
                                                  node.style.backdrop_blur};
            if (shape->contour) {
                effect.contour = *shape->contour;
            }
            ValidatePreparedSurfaceEffect(effect);

            result.push_back(std::move(effect));
        }
    }
    for (const auto &child : node.children) {
        CollectSurfaceEffects(*child, clips, result, placement);
    }
    if (clipped) {
        clips.pop_back();
    }
}

void Scene::AddInputRegion(const Shape &shape, const std::vector<Shape> &clips,
                           std::vector<RoundedShape> &output) const
{
    auto bounds = shape.Bounds();
    double radius = shape.rounded.corner_radius;
    bool identical = true;
    bool rectangular = radius == 0;
    bool has_contour = static_cast<bool>(shape.contour);
    for (const auto &clip : clips) {
        bounds = IntersectSurfaceRegionBounds(bounds, clip.Bounds());
        identical = identical && shape.rounded.bounds == clip.rounded.bounds;
        rectangular = rectangular && clip.rounded.corner_radius == 0;
        radius = std::max(radius, clip.rounded.corner_radius);
        has_contour = has_contour || static_cast<bool>(clip.contour);
    }
    if (!has_contour && (bounds.width <= 0 || bounds.height <= 0)) {
        return;
    }
    if (!has_contour) {
        if (clips.empty() || identical || rectangular) {
            AppendInputRegion({bounds, rectangular ? 0 : radius}, output);
            return;
        }

        std::vector<RoundedShape> intersection;
        intersection.reserve(clips.size() + 1);
        for (const auto &clip : clips) {
            intersection.push_back(clip.rounded);
        }
        intersection.push_back(shape.rounded);
        AppendInputMask(contracts::RasterizeRoundedIntersection(intersection), output);
        return;
    }

    std::vector<Shape> intersection;
    intersection.reserve(clips.size() + 1);
    AppendUniqueShape(shape, intersection);
    for (const auto &clip : clips) {
        AppendUniqueShape(clip, intersection);
    }
    std::vector<contracts::Contour> contours;
    std::vector<RoundedShape> rounded;
    for (const auto &item : intersection) {
        if (item.contour) {
            contours.push_back(*item.contour);
        } else {
            rounded.push_back(item.rounded);
        }
    }

    const Rect viewport{0, 0, viewport_.width, viewport_.height};
    AppendInputMask(contracts::RasterizeContourIntersection(contours, viewport, rounded), output);
}

void Scene::CollectInputRegions(const Node &node, std::vector<Shape> &clips,
                                std::vector<RoundedShape> &output,
                                const SceneRegionPlacement *placement) const
{
    if (node.kind == Kind::Visual || !IsVisible(node) || node.bounds.width <= 0 ||
        node.bounds.height <= 0 || (HasPopupSurfaceAdoption() && node.id == active_popup_) ||
        (placement && placement->close_popup && IsPopupKind(node.kind))) {
        return;
    }
    const bool clipped = IsPopupKind(node.kind) || node.kind == Kind::ScrollView ||
                         node.style.clip || node.style.overflow == "clip";
    if (clipped) {
        clips.push_back(RegionShape(node, placement));
    }
    const bool popup_open = (!placement || !placement->close_popup) && PopupToken();
    const bool material = (&node == root_.get() && (popup_open || OwnerModalToken())) ||
                          IsPopupKind(node.kind) || node.kind == Kind::ScrollView ||
                          IsInteractionOwner(node.kind) || node.style.input_shape == "bounds" ||
                          node.style.background.a || node.style.backdrop_blur > 0 ||
                          !node.action.empty() || node.kind == Kind::Image;
    if (material) {
        AddInputRegion(RegionShape(node, placement), clips, output);
    }
    // A material clip already covers all visible descendants. A visible
    // overflow child can extend the union beyond its parent's region.
    if (!(material && clipped)) {
        for (const auto &child : node.children) {
            CollectInputRegions(*child, clips, output, placement);
        }
    }
    if (clipped) {
        clips.pop_back();
    }
}

std::vector<contracts::SurfaceInputRegion>
Scene::PrepareScrolledRegions(const SceneRegionPlacement &placement) const
{
    std::vector<contracts::SurfaceEffectRegion> effects;
    std::vector<Shape> effect_clips{{{{0, 0, viewport_.width, viewport_.height}, 0}, {}}};
    CollectSurfaceEffects(*root_, effect_clips, effects, &placement);
    if (effects.size() > 8) {
        throw std::length_error("Surface effect region limit is 8");
    }

    std::vector<RoundedShape> input;
    std::vector<Shape> input_clips;
    CollectInputRegions(*root_, input_clips, input, &placement);
    return input;
}

std::vector<contracts::SurfaceEffectRegion> Scene::SurfaceEffects() const
{
    std::vector<contracts::SurfaceEffectRegion> result;
    std::vector<Shape> clips{{{{0, 0, viewport_.width, viewport_.height}, 0}, {}}};

    if (root_) {
        CollectSurfaceEffects(*root_, clips, result);
    }
    if (result.size() > 8) {
        throw std::length_error("Surface effect region limit is 8");
    }
    return result;
}

const std::vector<contracts::SurfaceInputRegion> &Scene::InputRegions() const
{
    if (!input_dirty_) {
        return input_regions_;
    }

    std::vector<RoundedShape> prepared;
    std::vector<Shape> clips;
    if (root_) {
        CollectInputRegions(*root_, clips, prepared);
    }

    input_regions_.swap(prepared);
    input_dirty_ = false;
    return input_regions_;
}

} // namespace prism::runtime
