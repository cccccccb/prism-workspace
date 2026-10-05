#include "scene_p.hpp"
#include <stdexcept>

namespace prism::runtime {
namespace {
using Shape = contracts::SurfaceInputRegion;
using Rect = contracts::LogicalRect;

Shape NormalizeShape(Shape shape)
{
    shape.corner_radius =
        std::clamp(shape.corner_radius, 0.0, std::min(shape.bounds.width, shape.bounds.height) / 2);
    return shape;
}

bool SameBounds(Rect a, Rect b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

bool InsideRoundedShape(contracts::LogicalPoint point, Shape shape)
{
    const auto &b = shape.bounds;
    if (point.x < b.x || point.y < b.y || point.x > b.x + b.width || point.y > b.y + b.height) {
        return false;
    }
    const auto r = shape.corner_radius;
    const auto x = std::clamp(point.x, b.x + r, b.x + b.width - r);
    const auto y = std::clamp(point.y, b.y + r, b.y + b.height - r);
    return (point.x - x) * (point.x - x) + (point.y - y) * (point.y - y) <= r * r + 1e-7;
}

bool ContainsRoundedShape(Shape outer, Rect box)
{
    return InsideRoundedShape({box.x, box.y}, outer) &&
           InsideRoundedShape({box.x + box.width, box.y}, outer) &&
           InsideRoundedShape({box.x, box.y + box.height}, outer) &&
           InsideRoundedShape({box.x + box.width, box.y + box.height}, outer);
}

std::optional<Shape> IntersectEffectShapes(Shape a, Shape b)
{
    const double x = std::max(a.bounds.x, b.bounds.x), y = std::max(a.bounds.y, b.bounds.y);
    const Rect box{
        x, y, std::max(0.0, std::min(a.bounds.x + a.bounds.width, b.bounds.x + b.bounds.width) - x),
        std::max(0.0, std::min(a.bounds.y + a.bounds.height, b.bounds.y + b.bounds.height) - y)};
    if (box.width == 0 || box.height == 0) {
        return std::nullopt;
    }
    if (SameBounds(a.bounds, b.bounds)) {
        return Shape{box, std::max(a.corner_radius, b.corner_radius)};
    }
    if (SameBounds(box, a.bounds) && ContainsRoundedShape(b, a.bounds)) {
        return a;
    }
    if (SameBounds(box, b.bounds) && ContainsRoundedShape(a, b.bounds)) {
        return b;
    }
    if (ContainsRoundedShape(a, box) && ContainsRoundedShape(b, box)) {
        return Shape{box, 0};
    }
    // The v1 protocol has one uniform-radius rounded rectangle. Cropping
    // its curved corners, or combining offset curved clips, can produce
    // asymmetric masks that cannot be transmitted faithfully.
    throw std::runtime_error(
        "Unsupported backdrop clipping: intersection is not a v1 rounded rectangle");
}

bool SameInputBounds(Rect a, Rect b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

Rect IntersectInputBounds(Rect a, Rect b)
{
    const auto x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return Rect{x, y, std::max(0.0, std::min(a.x + a.width, b.x + b.width) - x),
                std::max(0.0, std::min(a.y + a.height, b.y + b.height) - y)};
}

std::pair<double, double> ShapeSpan(Shape region, double y)
{
    const auto &b = region.bounds;
    const double r = std::clamp(region.corner_radius, 0.0, std::min(b.width, b.height) / 2);
    const double edge = std::min(y - b.y, b.y + b.height - y);
    double inset = 0;
    if (edge < r) {
        inset = r - std::sqrt(std::max(0.0, r * r - (r - edge) * (r - edge)));
    }
    return std::pair{b.x + inset, b.x + b.width - inset};
}
} // namespace

void Scene::CollectSurfaceEffects(const Node &node, std::vector<Shape> &clips,
                                  std::vector<contracts::SurfaceEffectRegion> &result) const
{
    if (node.kind == Kind::Visual || !IsVisible(node) || node.bounds.width <= 0 ||
        node.bounds.height <= 0) {
        return;
    }
    const bool clipped = node.style.clip || node.style.overflow == "clip";
    if (clipped) {
        clips.push_back(NormalizeShape({node.bounds, node.style.radius}));
    }
    if (node.style.backdrop_blur > 0) {
        std::optional<Shape> shape = NormalizeShape({node.bounds, node.style.radius});
        for (const auto &clip : clips) {
            shape = IntersectEffectShapes(*shape, clip);
            if (!shape) {
                break;
            }
        }
        if (shape) {
            const auto &b = shape->bounds;
            if (std::abs(b.x) > 8192 || std::abs(b.y) > 8192 || b.width > 8192 || b.height > 8192) {
                throw std::runtime_error(
                    "Unsupported backdrop bounds: v1 maximum is 8192 logical pixels");
            }
            result.push_back({b, shape->corner_radius, node.style.backdrop_blur});
        }
    }
    for (const auto &child : node.children) {
        CollectSurfaceEffects(*child, clips, result);
    }
    if (clipped) {
        clips.pop_back();
    }
}

void Scene::AddInputRegion(Shape shape, const std::vector<Shape> &clips) const
{
    auto bounds = shape.bounds;
    double radius = shape.corner_radius;
    bool identical = true, rectangular = radius == 0;
    for (const auto &clip : clips) {
        bounds = IntersectInputBounds(bounds, clip.bounds);
        identical = identical && SameInputBounds(shape.bounds, clip.bounds);
        rectangular = rectangular && clip.corner_radius == 0;
        radius = std::max(radius, clip.corner_radius);
    }
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    if (clips.empty() || identical || rectangular) {
        input_regions_.push_back({bounds, rectangular ? 0 : radius});
        return;
    }
    // Arbitrary rounded intersections are not themselves rounded rects.
    // Resolve the exact logical-pixel input mask once, using the same
    // pixel-center convention as the Wayland region rasterizer.

    for (int y = static_cast<int>(std::ceil(bounds.y));
         y < static_cast<int>(std::floor(bounds.y + bounds.height)); ++y) {
        auto [left, right] = ShapeSpan(shape, y + .5);
        for (const auto &clip : clips) {
            const auto [a, b] = ShapeSpan(clip, y + .5);
            left = std::max(left, a);
            right = std::min(right, b);
        }
        const double first = std::ceil(left), last = std::floor(right);
        if (last > first) {
            input_regions_.push_back({{first, double(y), last - first, 1}, 0});
        }
    }
}

void Scene::CollectInputRegions(const Node &node, std::vector<Shape> &clips) const
{
    if (node.kind == Kind::Visual || !IsVisible(node) || node.bounds.width <= 0 ||
        node.bounds.height <= 0) {
        return;
    }
    const bool clipped = node.style.clip || node.style.overflow == "clip";
    if (clipped) {
        clips.push_back({node.bounds, node.style.radius});
    }
    const bool material = node.kind == Kind::InteractionTarget ||
                          node.style.input_shape == "bounds" || node.style.background.a ||
                          node.style.backdrop_blur > 0 || !node.action.empty() ||
                          node.kind == Kind::Image;
    if (material) {
        AddInputRegion({node.bounds, node.style.radius}, clips);
    }
    // A material clip already covers all visible descendants. A visible
    // overflow child can extend the union beyond its parent's region.
    if (!(material && clipped)) {
        for (const auto &child : node.children) {
            CollectInputRegions(*child, clips);
        }
    }
    if (clipped) {
        clips.pop_back();
    }
}

std::vector<contracts::SurfaceEffectRegion> Scene::SurfaceEffects() const
{
    std::vector<contracts::SurfaceEffectRegion> result;

    std::vector<Shape> clips{{{0, 0, viewport_.width, viewport_.height}, 0}};

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
    input_regions_.clear();

    std::vector<Shape> clips;

    if (root_) {
        CollectInputRegions(*root_, clips);
    }
    input_dirty_ = false;
    return input_regions_;
}

} // namespace prism::runtime
