#include "prism/runtime/theme_tokens.hpp"
#include "scene_contour_p.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace prism::runtime {
namespace {
bool ValidTokenName(const std::string &name)
{
    return !name.empty() && name.size() <= 64 &&
           std::all_of(name.begin(), name.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '-';
           });
}

double ResolveNumber(const ContourNumber &number, const contracts::ThemeSnapshot *theme)
{
    if (const auto *literal = std::get_if<double>(&number)) {
        return *literal;
    }
    const auto &name = std::get<ContourThemeNumber>(number).name;
    if (!theme || !ValidTokenName(name)) {
        throw std::invalid_argument("Contour recipe requires a valid Number theme token: " + name);
    }
    const auto token = ResolveThemeToken(*theme, name);
    const auto *value = token ? std::get_if<double>(&*token) : nullptr;
    if (!value) {
        throw std::invalid_argument("Missing or non-number Contour recipe token: " + name);
    }
    return *value;
}

bool ShapeContains(const SnapshotNode &node, contracts::LogicalPoint point)
{
    return node.contour ? contracts::ContourContains(*node.contour, point)
                        : contracts::RoundedRegionContains(point, {node.bounds, node.style.radius});
}

bool VisibleAnchorPoint(const SceneSnapshot &snapshot, contracts::NodeId anchor,
                        contracts::LogicalPoint point)
{
    const auto *node = &snapshot.Get(anchor);
    if (!node->style.visible || !ShapeContains(*node, point)) {
        return false;
    }
    while (node->parent) {
        node = &snapshot.Get(node->parent);
        if (!node->style.visible) {
            return false;
        }
        if ((node->style.clip || node->style.overflow == "clip" ||
             node->kind == Kind::ScrollView) &&
            !ShapeContains(*node, point)) {
            return false;
        }
    }
    return true;
}

contracts::PanelContourRequest Request(const SceneSnapshot &snapshot, const SnapshotNode &node)
{
    contracts::PanelContourRequest request{{node.bounds.width, node.bounds.height},
                                           *node.contour_spec};
    const auto &placement = node.popup_placement;
    if (!placement || placement->side == PopupSide::EdgePanel) {
        return request;
    }
    const auto &anchor = placement->effective_anchor;
    const double anchor_left = anchor.x - node.bounds.x;
    const auto center =
        contracts::PanelAttachmentCenter(request.body, request.spec, anchor_left + anchor.width / 2,
                                         anchor_left, anchor_left + anchor.width);
    if (!center || !VisibleAnchorPoint(snapshot, node.popup_anchor,
                                       {node.bounds.x + *center, anchor.y + anchor.height / 2})) {
        return request;
    }

    request.center = *center;
    request.edge = placement->side == PopupSide::Below ? contracts::PanelAttachmentEdge::Top
                                                       : contracts::PanelAttachmentEdge::Bottom;
    return request;
}
} // namespace

contracts::PanelContourSpec ResolveContourRecipe(const AttachedPanelRecipe &recipe,
                                                 const contracts::ThemeSnapshot *theme)
{
    if (recipe.fallback != ContourFallback::Detached) {
        throw std::invalid_argument("Unsupported Contour recipe fallback");
    }
    contracts::PanelContourSpec spec{ResolveNumber(recipe.radius, theme),
                                     ResolveNumber(recipe.neck_width, theme),
                                     ResolveNumber(recipe.neck_height, theme), recipe.neck_shape};
    contracts::ValidatePanelContourSpec(spec);
    return spec;
}

void PrepareAttachedPanelContours(SceneSnapshot &snapshot)
{
    for (auto &node : snapshot.nodes) {
        if (!node.id || !node.contour_spec || !node.style.visible || node.bounds.width <= 0 ||
            node.bounds.height <= 0) {
            continue;
        }
        const auto request = Request(snapshot, node);
        if (!node.contour_prepared || node.contour_request != request) {
            node.contour_prepared =
                std::make_shared<const contracts::Contour>(contracts::PreparePanelContour(request));
            node.contour_request = request;
        }
        node.contour =
            PlaceSceneContour(*node.contour_prepared, {node.bounds.x, node.bounds.y}, node.contour);
    }
}
} // namespace prism::runtime
