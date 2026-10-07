#include "popup_surface_p.hpp"
#include "prism/contracts/display_list_validation.hpp"
#include "prism/contracts/rounded_region.hpp"
#include "prism/runtime/display_list_builder.hpp"
#include "prism/runtime/layout_engine.hpp"
#include "prism/runtime/render_tree.hpp"
#include "scene_contour_p.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>

namespace prism::runtime {
namespace {
using Rect = contracts::LogicalRect;

bool ShapeContains(const SnapshotNode &node, contracts::LogicalPoint point,
                   bool include_edges = false)
{
    return node.contour ? contracts::ContourContains(*node.contour, point)
                        : contracts::RoundedRegionContains(point, {node.bounds, node.style.radius},
                                                           include_edges);
}

bool VisibleAnchorPoint(const SceneSnapshot &snapshot, contracts::NodeId id,
                        contracts::LogicalPoint point)
{
    const auto *node = &snapshot.Get(id);
    if (!node->style.visible || !ShapeContains(*node, point)) {
        return false;
    }
    while (node->parent) {
        node = &snapshot.Get(node->parent);
        if (!node->style.visible || ((node->style.clip || node->style.overflow == "clip" ||
                                      node->kind == Kind::ScrollView) &&
                                     !ShapeContains(*node, point))) {
            return false;
        }
    }
    return true;
}

PopupPlacement FinalPlacement(const SceneSnapshot &snapshot, const PopupSurfaceRequest &request,
                              Rect window)
{
    PopupPlacement placement{window, PopupSide::EdgePanel, false, false, request.anchor};
    const auto &popup = snapshot.Get(request.active_node);
    if (!popup.contour_spec) {
        return placement;
    }
    // Place the canonical, quantized attachment against the integer native
    // window edge; independent rounding of body and neck must not overshoot it.
    const double neck = std::round(contracts::PanelNeckHeight(*popup.contour_spec) * 256) / 256;
    if (neck <= 0 || window.height <= neck) {
        return placement;
    }

    Rect body = window;
    if (window.y >= request.anchor.y + request.anchor.height) {
        placement.side = PopupSide::Below;
        body.y += neck;
    } else if (window.y + window.height <= request.anchor.y) {
        placement.side = PopupSide::Above;
    } else {
        return placement;
    }
    body.height -= neck;
    const double left = request.anchor.x - body.x;
    const auto center = contracts::PanelAttachmentCenter(
        {body.width, body.height}, *popup.contour_spec, left + request.anchor.width / 2, left,
        left + request.anchor.width);
    if (!center ||
        !VisibleAnchorPoint(snapshot, request.trigger,
                            {body.x + *center, request.anchor.y + request.anchor.height / 2})) {
        placement.side = PopupSide::EdgePanel;
        return placement;
    }
    placement.bounds = body;
    return placement;
}

void ValidateSubtree(const SceneSnapshot &snapshot, contracts::NodeId id)
{
    const auto &node = snapshot.Get(id);
    if (!node.style.visible) {
        return;
    }
    if (node.kind == Kind::TextField || node.kind == Kind::TextArea) {
        throw std::invalid_argument("Popup surface export does not yet prepare text editors");
    }
    const auto &p = node.presentation;
    if (node.kind != Kind::Visual &&
        (p.translate_x != 0 || p.translate_y != 0 || p.scale_x != 1 || p.scale_y != 1)) {
        throw std::invalid_argument("Popup surface export cannot transform interactive geometry");
    }
    for (auto child : node.children) {
        ValidateSubtree(snapshot, child);
    }
}

Rect PaintBounds(const SnapshotNode &popup)
{
    Rect bounds = popup.contour ? contracts::ContourBounds(*popup.contour) : popup.bounds;
    double left = bounds.x - 2;
    double top = bounds.y - 2;
    double right = bounds.x + bounds.width + 2;
    double bottom = bounds.y + bounds.height + 2;
    if (popup.style.shadow_color.a) {
        const double pad = std::ceil(3 * popup.style.shadow_blur) + 4;
        left = std::min(left, bounds.x - pad);
        right = std::max(right, bounds.x + bounds.width + pad);
        top = std::min(top, bounds.y + popup.style.shadow_y - pad);
        bottom = std::max(bottom, bounds.y + bounds.height + popup.style.shadow_y + pad);
    }
    return {std::floor(left), std::floor(top), std::ceil(right) - std::floor(left),
            std::ceil(bottom) - std::floor(top)};
}

void ValidateAncestorPaint(const SceneSnapshot &snapshot, const SnapshotNode &popup, Rect paint)
{
    auto parent = popup.parent;
    while (parent) {
        const auto &node = snapshot.Get(parent);
        const auto &p = node.presentation;
        if (node.kind == Kind::Visual || p.translate_x != 0 || p.translate_y != 0 ||
            p.scale_x != 1 || p.scale_y != 1 || p.opacity != 1) {
            throw std::invalid_argument("Popup surface export cannot preserve ancestor transforms");
        }
        if (node.style.clip || node.style.overflow == "clip" || node.kind == Kind::ScrollView) {
            if (node.contour) {
                throw std::invalid_argument(
                    "Popup surface export cannot preserve ancestor contours");
            }
            for (auto corner : std::array<contracts::LogicalPoint, 4>{
                     {{paint.x, paint.y},
                      {paint.x + paint.width, paint.y},
                      {paint.x, paint.y + paint.height},
                      {paint.x + paint.width, paint.y + paint.height}}}) {
                if (!ShapeContains(node, corner, true)) {
                    throw std::invalid_argument(
                        "Popup surface export would lose ancestor clipping");
                }
            }
        }
        parent = node.parent;
    }
}

void TranslateSubtree(SceneSnapshot &snapshot, contracts::NodeId id, contracts::LogicalPoint origin)
{
    auto &node = snapshot.Get(id);
    node.bounds.x -= origin.x;
    node.bounds.y -= origin.y;
    if (node.contour) {
        auto contour = *node.contour;
        for (auto &point : contour.points) {
            point.x -= origin.x;
            point.y -= origin.y;
        }
        contracts::ValidateContour(contour);
        node.contour = std::make_shared<const contracts::Contour>(std::move(contour));
    }
    for (auto child : node.children) {
        TranslateSubtree(snapshot, child, origin);
    }
}
} // namespace

PopupSurfacePlan PreparePopupSurfaceValues(const PopupSurfaceRequest &request,
                                           const PopupSurfaceConfigure &configure,
                                           const ShapeText &shaper, contracts::ResourceId font,
                                           const PopupSurfacePrepared *previous)
{
    auto snapshot = *request.source->snapshot;
    ValidateSubtree(snapshot, request.active_node);
    Rect window = configure.window_bounds;
    window.x += request.parent_window_geometry.x;
    window.y += request.parent_window_geometry.y;
    const auto placement = FinalPlacement(snapshot, request, window);

    const bool layout_reused =
        ReusePopupLayout(snapshot, request, configure, placement.bounds, previous);
    if (!layout_reused) {
        LayoutEngine::ComputeSubtree(snapshot, request.active_node, placement.bounds, shaper);
    }
    auto &popup = snapshot.Get(request.active_node);
    popup.popup_placement = placement;
    ApplyPopupControlVisuals(snapshot, *request.source);
    PrepareSceneContours(snapshot);
    const auto functional = popup.contour ? contracts::ContourBounds(*popup.contour) : popup.bounds;
    if (functional.x < window.x || functional.y < window.y ||
        functional.x + functional.width > window.x + window.width ||
        functional.y + functional.height > window.y + window.height) {
        throw std::invalid_argument("Popup functional contour exceeds native window geometry");
    }
    const Rect paint = PaintBounds(popup);
    if (!std::isfinite(paint.x) || !std::isfinite(paint.y) || !std::isfinite(paint.width) ||
        !std::isfinite(paint.height) || paint.width <= 0 || paint.height <= 0 ||
        paint.width > 4096 || paint.height > 4096) {
        throw std::invalid_argument("Popup surface paint buffer exceeds supported bounds");
    }
    ValidateAncestorPaint(snapshot, popup, paint);

    PopupSurfacePlan plan;
    plan.request = request;
    plan.configure_generation = configure.configure_generation;
    plan.body_bounds = placement.bounds;
    plan.surface_origin = {paint.x, paint.y};
    plan.buffer_size = {static_cast<std::uint32_t>(paint.width),
                        static_cast<std::uint32_t>(paint.height)};
    plan.window_geometry = {window.x - paint.x, window.y - paint.y, window.width, window.height};
    plan.body_geometry = {placement.bounds.x - paint.x, placement.bounds.y - paint.y,
                          placement.bounds.width, placement.bounds.height};
    if (plan.window_geometry.x < 0 || plan.window_geometry.y < 0 ||
        plan.window_geometry.x + plan.window_geometry.width > paint.width ||
        plan.window_geometry.y + plan.window_geometry.height > paint.height) {
        throw std::invalid_argument("Popup native window geometry exceeds paint buffer");
    }
    plan.font = font;
    auto resolved_layout = std::make_shared<const SceneSnapshot>(snapshot);
    TranslateSubtree(snapshot, request.active_node, plan.surface_origin);
    snapshot.root = request.active_node;
    popup.parent = {};

    const contracts::LogicalSize viewport{paint.width, paint.height};
    plan.input_snapshot = PreparePopupInput(snapshot, *request.source, viewport);
    plan.input_regions = PreparePopupInputRegions(snapshot, request.active_node, viewport);
    plan.effect_regions = PreparePopupEffects(snapshot, request.active_node, viewport);
    auto tree = RenderTreeBuilder::Build(snapshot);
    auto list = DisplayListBuilder::Build(tree, request.source->window, font,
                                          configure.configure_generation);
    contracts::ValidateDisplayList(list);
    plan.display_list = std::make_shared<const contracts::DisplayList>(std::move(list));
    auto prepared = std::make_shared<PopupSurfacePrepared>();
    prepared->values = plan;
    prepared->configure = configure;
    prepared->layout = std::move(resolved_layout);
    prepared->layout_reused = layout_reused;
    plan.prepared = std::move(prepared);
    return plan;
}
} // namespace prism::runtime
