#include "prism/runtime/layout_engine.hpp"
#include "prism/contracts/rounded_region.hpp"
#include "prism/runtime/popup.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace prism::runtime {
namespace {
using Size = contracts::LogicalSize;
using Rect = contracts::LogicalRect;

double PadX(const Style &s)
{
    return s.padding_x < 0 ? s.padding : s.padding_x;
}

double PadY(const Style &s)
{
    return s.padding_y < 0 ? s.padding : s.padding_y;
}

bool Container(Kind kind)
{
    return kind == Kind::Row || kind == Kind::Column || kind == Kind::Box || IsFloatingKind(kind) ||
           IsInteractionOwner(kind) || IsChoiceGroup(kind) || kind == Kind::ScrollView ||
           kind == Kind::Visual;
}

ShapedText ShapeLines(std::string_view text, const Style &style, const ShapeText &shaper)
{
    ShapedText result;
    std::size_t start = 0;
    for (;;) {
        const auto end = text.find_first_of("\r\n", start);
        const auto line = text.substr(start, end == std::string_view::npos ? end : end - start);
        auto shaped = shaper(line, style.font_size);
        if (!std::isfinite(shaped.width) || !std::isfinite(shaped.height) || shaped.width < 0 ||
            shaped.height < 0) {
            throw std::invalid_argument("Invalid line shaping metrics");
        }

        const double height = std::max(style.line_height, shaped.height);
        const double offset = result.height + (height - shaped.height) / 2;
        for (auto glyph : shaped.glyphs) {
            glyph.origin.y += offset;
            result.glyphs.push_back(glyph);
        }
        result.width = std::max(result.width, shaped.width);
        result.height += height;

        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
        if (text[end] == '\r' && start < text.size() && text[start] == '\n') {
            ++start;
        }
    }
    return result;
}

Size Measure(SceneSnapshot &snapshot, contracts::NodeId id, const ShapeText &shaper)
{
    auto &node = snapshot.Get(id);
    if (!node.style.visible) {
        return {};
    }
    Size natural{};
    if (node.kind == Kind::Text) {
        node.shaped = ShapeLines(node.text, node.style, shaper);
        natural = {node.shaped.width, node.shaped.height};
    } else if (node.kind == Kind::Image && node.image_ready) {
        natural = node.intrinsic_size;
    } else if (node.kind == Kind::Icon || node.kind == Kind::IconButton) {
        natural = {24, 24};
    } else if (node.kind == Kind::Toggle) {
        natural = {38, 22};
    } else if (node.kind == Kind::Progress) {
        natural = {80, 5};
    } else if (node.kind == Kind::Separator) {
        natural = {1, 24};
    } else {
        std::size_t visible_count = 0;
        for (auto child_id : node.children) {
            if (!snapshot.Get(child_id).style.visible ||
                IsFloatingKind(snapshot.Get(child_id).kind)) {
                continue;
            }
            ++visible_count;
            auto child = Measure(snapshot, child_id, shaper);
            const auto inset = snapshot.Get(child_id).style.inset * 2;
            child.width += inset;
            child.height += inset;
            if ((node.kind == Kind::Row || node.kind == Kind::SegmentGroup)) {
                natural.width += child.width;
                natural.height = std::max(natural.height, child.height);
            } else if ((node.kind == Kind::Column || node.kind == Kind::RadioGroup)) {
                natural.height += child.height;
                natural.width = std::max(natural.width, child.width);
            } else {
                natural.width = std::max(natural.width, child.width);
                natural.height = std::max(natural.height, child.height);
            }
        }
        if (visible_count) {
            const auto gaps = node.style.spacing * (visible_count - 1);
            if ((node.kind == Kind::Row || node.kind == Kind::SegmentGroup)) {
                natural.width += gaps;
            }
            if ((node.kind == Kind::Column || node.kind == Kind::RadioGroup)) {
                natural.height += gaps;
            }
        }
    }
    natural.width += 2 * PadX(node.style);
    natural.height += 2 * PadY(node.style);
    if (node.style.width > 0) {
        natural.width = node.style.width;
    }
    if (node.style.height > 0) {
        natural.height = node.style.height;
    }
    node.intrinsic_size = natural;
    return natural;
}

double Offset(std::string_view align, double available, double length)
{
    if (align == "center") {
        return std::max(0.0, (available - length) / 2);
    }
    if (align == "end" || align == "right") {
        return std::max(0.0, available - length);
    }
    return 0;
}

void Place(SceneSnapshot &snapshot, contracts::NodeId id, Rect bounds)
{
    auto &node = snapshot.Get(id);
    if (!node.style.visible) {
        return;
    }
    node.bounds = bounds;
    if (!Container(node.kind)) {
        return;
    }
    std::vector<contracts::NodeId> children;
    for (auto child : node.children) {
        if (snapshot.Get(child).style.visible && !IsFloatingKind(snapshot.Get(child).kind)) {
            children.push_back(child);
        }
    }
    const double px = PadX(node.style), py = PadY(node.style);
    Rect inner{bounds.x + px, bounds.y + py, std::max(0.0, bounds.width - 2 * px),
               std::max(0.0, bounds.height - 2 * py)};
    if (node.kind == Kind::ScrollView) {
        node.scroll_content_height = 0;
        for (const auto child_id : children) {
            const auto &child = snapshot.Get(child_id);
            if (child.kind != Kind::Visual) {
                node.scroll_content_height = child.intrinsic_size.height + 2 * child.style.inset;
            }
        }
        node.scroll_offset = std::clamp(node.scroll_offset, 0.0,
                                        std::max(0.0, node.scroll_content_height - bounds.height));
        for (const auto child_id : children) {
            const auto &child = snapshot.Get(child_id);
            if (child.kind == Kind::Visual) {
                Place(snapshot, child_id, bounds);
                continue;
            }
            const double margin = child.style.inset;
            const double width = std::max(0.0, bounds.width - 2 * margin);
            Place(snapshot, child_id,
                  {bounds.x + margin, bounds.y + margin - node.scroll_offset,
                   child.style.width > 0 ? std::min(width, child.style.width) : width,
                   child.intrinsic_size.height});
        }
        return;
    }
    if (node.kind == Kind::Box || IsFloatingKind(node.kind) || IsInteractionOwner(node.kind) ||
        node.kind == Kind::Visual) {
        double centered_width = 0;
        for (auto child_id : children) {
            const auto &child = snapshot.Get(child_id);
            if (child.style.anchor == "center") {
                centered_width =
                    std::max(centered_width, child.style.width > 0 ? child.style.width
                                                                   : child.intrinsic_size.width);
            }
        }
        for (auto child_id : children) {
            const auto &child = snapshot.Get(child_id);
            const auto &style = child.style;
            const double margin = style.inset;
            const double available_w = std::max(0.0, inner.width - 2 * margin),
                         available_h = std::max(0.0, inner.height - 2 * margin);
            const double side_limit =
                centered_width > 0 && (style.anchor == "left" || style.anchor == "right")
                    ? std::max(0.0, (available_w - centered_width) / 2 - node.style.spacing)
                    : available_w;
            const bool anchored = style.anchor != "fill";
            const double w = std::min(side_limit, style.width > 0 ? style.width
                                                  : anchored      ? child.intrinsic_size.width
                                                                  : available_w);
            const double h =
                std::min(available_h, style.height > 0           ? style.height
                                      : child.kind == Kind::Text ? child.intrinsic_size.height
                                                                 : available_h);
            const auto horizontal = style.anchor == "left"     ? "start"
                                    : style.anchor == "center" ? "center"
                                    : style.anchor == "right"  ? "end"
                                                               : node.style.align.c_str();
            Place(snapshot, child_id,
                  {inner.x + margin + Offset(horizontal, available_w, w),
                   inner.y + margin + Offset(node.style.justify, available_h, h), w, h});
        }
        return;
    }
    const bool row = (node.kind == Kind::Row || node.kind == Kind::SegmentGroup);
    const double main = row ? inner.width : inner.height, cross = row ? inner.height : inner.width;
    double gap = node.style.spacing;
    double fixed = gap * (children.empty() ? 0 : children.size() - 1), total_weight = 0;
    std::vector<double> lengths, weights;
    for (auto child_id : children) {
        const auto &child = snapshot.Get(child_id);
        const double explicit_size = row ? child.style.width : child.style.height;
        const double weight = child.style.flex > 0 ? child.style.flex
                              : explicit_size == 0 && Container(child.kind) &&
                                      !IsChoiceGroup(child.kind) && child.kind != Kind::Slider
                                  ? 1
                                  : 0;
        const double length = weight > 0 ? 0
                              : row      ? child.intrinsic_size.width
                                         : child.intrinsic_size.height;
        fixed += length + 2 * child.style.inset;
        total_weight += weight;
        lengths.push_back(length);
        weights.push_back(weight);
    }
    const double remaining = std::max(0.0, main - fixed);
    if (total_weight > 0) {
        for (std::size_t i = 0; i < lengths.size(); ++i) {
            lengths[i] += remaining * weights[i] / total_weight;
        }
    }
    double cursor = (row ? inner.x : inner.y) +
                    Offset(node.style.justify, main, fixed + (total_weight > 0 ? remaining : 0));
    if (total_weight == 0 && node.style.justify == "spaceBetween" && lengths.size() > 1) {
        gap += remaining / (lengths.size() - 1);
    }
    for (std::size_t i = 0; i < children.size(); ++i) {
        const auto child_id = children[i];
        const auto &child = snapshot.Get(child_id);
        const double margin = child.style.inset;
        const double explicit_cross = row ? child.style.height : child.style.width;
        const double available_cross = std::max(0.0, cross - 2 * margin);
        const double natural_cross = row ? child.intrinsic_size.height : child.intrinsic_size.width;
        // An automatic image retains its decoded size in flow containers;
        // explicit dimensions (or a Card fill slot) select a scaling target.
        const double other =
            std::min(available_cross, explicit_cross > 0 ? explicit_cross
                                      : node.style.align == "stretch" && child.kind != Kind::Image
                                          ? available_cross
                                          : natural_cross);
        const double start =
            (row ? inner.y : inner.x) + margin + Offset(node.style.align, available_cross, other);
        Place(snapshot, child_id,
              row ? Rect{cursor + margin, start, lengths[i], other}
                  : Rect{start, cursor + margin, other, lengths[i]});
        cursor += lengths[i] + 2 * margin + gap;
    }
}

Rect ClippedAnchor(const SceneSnapshot &snapshot, contracts::NodeId id)
{
    auto anchor = snapshot.Get(id).bounds;
    auto parent = snapshot.Get(id).parent;
    while (parent) {
        const auto &item = snapshot.Get(parent);
        if (item.style.clip || item.style.overflow == "clip" || item.kind == Kind::ScrollView) {
            const double right =
                std::min(anchor.x + anchor.width, item.bounds.x + item.bounds.width);
            const double bottom =
                std::min(anchor.y + anchor.height, item.bounds.y + item.bounds.height);
            anchor.x = std::max(anchor.x, item.bounds.x);
            anchor.y = std::max(anchor.y, item.bounds.y);
            anchor.width = std::max(0.0, right - anchor.x);
            anchor.height = std::max(0.0, bottom - anchor.y);
        }
        parent = item.parent;
    }
    return anchor;
}

struct TooltipTransform {
    double scale_x{1}, scale_y{1};
    double translate_x{}, translate_y{};
};

Rect TransformBounds(Rect bounds, const TooltipTransform &transform)
{
    return {bounds.x * transform.scale_x + transform.translate_x,
            bounds.y * transform.scale_y + transform.translate_y, bounds.width * transform.scale_x,
            bounds.height * transform.scale_y};
}

bool TooltipContentsFit(const SceneSnapshot &snapshot, contracts::NodeId id,
                        const SnapshotNode *parent = nullptr, TooltipTransform transform = {})
{
    const auto &node = snapshot.Get(id);
    if (!node.style.visible) {
        return true;
    }
    constexpr double epsilon = 0.001;
    const auto parent_transform = transform;
    if (node.kind == Kind::Visual) {
        const auto &value = node.presentation;
        const auto origin_x = node.bounds.x + node.bounds.width * value.origin_x;
        const auto origin_y = node.bounds.y + node.bounds.height * value.origin_y;
        transform.translate_x +=
            transform.scale_x * (value.translate_x + origin_x * (1 - value.scale_x));
        transform.translate_y +=
            transform.scale_y * (value.translate_y + origin_y * (1 - value.scale_y));
        transform.scale_x *= value.scale_x;
        transform.scale_y *= value.scale_y;
    }
    const auto bounds = TransformBounds(node.bounds, transform);
    if (bounds.width <= 0 || bounds.height <= 0) {
        return false;
    }
    if (parent) {
        const auto parent_bounds = TransformBounds(parent->bounds, parent_transform);
        if (bounds.x + epsilon < parent_bounds.x || bounds.y + epsilon < parent_bounds.y ||
            bounds.x + bounds.width > parent_bounds.x + parent_bounds.width + epsilon ||
            bounds.y + bounds.height > parent_bounds.y + parent_bounds.height + epsilon) {
            return false;
        }
        if (parent->kind == Kind::Tooltip || parent->style.clip ||
            parent->style.overflow == "clip") {
            const contracts::SurfaceInputRegion clip{parent->bounds, parent->style.radius};
            for (const auto point :
                 {contracts::LogicalPoint{bounds.x, bounds.y},
                  contracts::LogicalPoint{bounds.x + bounds.width, bounds.y},
                  contracts::LogicalPoint{bounds.x, bounds.y + bounds.height},
                  contracts::LogicalPoint{bounds.x + bounds.width, bounds.y + bounds.height}}) {
                const contracts::LogicalPoint local{
                    (point.x - parent_transform.translate_x) / parent_transform.scale_x,
                    (point.y - parent_transform.translate_y) / parent_transform.scale_y};
                if (!contracts::RoundedRegionContains(local, clip, true)) {
                    return false;
                }
            }
        }
    }
    if (node.kind == Kind::Text &&
        (node.shaped.width > node.bounds.width + epsilon ||
         node.shaped.height > node.bounds.height + epsilon ||
         (!node.text.empty() && (node.shaped.width <= 0 || node.shaped.height <= 0)))) {
        return false;
    }
    for (const auto child : node.children) {
        if (!TooltipContentsFit(snapshot, child, &node, transform)) {
            return false;
        }
    }
    return true;
}

bool TooltipFitsRootClip(const SnapshotNode &root, Rect bounds)
{
    if (!root.style.clip && root.style.overflow != "clip") {
        return true;
    }
    // Noninteractive hints cannot offer scrolling when an arbitrary authored
    // polygon clips their content. Keep them hidden in that unsupported case.
    if (root.contour_source || root.contour_spec) {
        return false;
    }
    const contracts::SurfaceInputRegion clip{root.bounds, root.style.radius};
    for (const auto point :
         {contracts::LogicalPoint{bounds.x, bounds.y},
          contracts::LogicalPoint{bounds.x + bounds.width, bounds.y},
          contracts::LogicalPoint{bounds.x, bounds.y + bounds.height},
          contracts::LogicalPoint{bounds.x + bounds.width, bounds.y + bounds.height}}) {
        if (!contracts::RoundedRegionContains(point, clip, true)) {
            return false;
        }
    }
    return true;
}

void PlaceTooltip(SceneSnapshot &snapshot, contracts::NodeId id, const ShapeText &shaper)
{
    auto &tooltip = snapshot.Get(id);
    if (!tooltip.tooltip_anchor) {
        tooltip.style.visible = false;
        return;
    }
    const auto size = Measure(snapshot, id, shaper);
    PopupPlacementRequest request{snapshot.Get(snapshot.root).bounds,
                                  ClippedAnchor(snapshot, tooltip.tooltip_anchor), size, size.width,
                                  size.height};
    request.horizontal_alignment = PopupHorizontalAlignment::Center;
    const auto placement = PlacePopup(request);
    if (!placement || placement->side == PopupSide::EdgePanel || placement->width_constrained ||
        placement->height_constrained ||
        !TooltipFitsRootClip(snapshot.Get(snapshot.root), placement->bounds)) {
        tooltip.style.visible = false;
        return;
    }

    Place(snapshot, id, placement->bounds);
    if (!TooltipContentsFit(snapshot, id)) {
        tooltip.bounds = {};
        tooltip.style.visible = false;
    }
}
} // namespace

void LayoutEngine::Compute(SceneSnapshot &snapshot, contracts::LogicalSize viewport,
                           const ShapeText &shaper)
{
    if (!snapshot.root || !shaper) {
        throw std::invalid_argument("Layout needs a root and text shaper");
    }
    for (auto &node : snapshot.nodes) {
        node.bounds = {};
        node.popup_placement.reset();
    }
    Measure(snapshot, snapshot.root, shaper);
    const auto inset = snapshot.Get(snapshot.root).style.inset;
    Place(snapshot, snapshot.root,
          {inset, inset, std::max(0.0, viewport.width - 2 * inset),
           std::max(0.0, viewport.height - 2 * inset)});
    for (auto id : snapshot.Get(snapshot.root).children) {
        auto &popup = snapshot.Get(id);
        if (popup.kind == Kind::Tooltip && popup.style.visible) {
            PlaceTooltip(snapshot, id, shaper);
            continue;
        }
        if (!IsPopupKind(popup.kind) || !popup.style.visible) {
            continue;
        }
        const auto size = Measure(snapshot, id, shaper);
        PopupPlacementRequest request{snapshot.Get(snapshot.root).bounds,
                                      ClippedAnchor(snapshot, popup.popup_anchor), size,
                                      std::min(160.0, size.width), std::min(80.0, size.height)};
        if (popup.contour_spec) {
            request.gap += contracts::PanelNeckHeight(*popup.contour_spec);
            request.horizontal_alignment = PopupHorizontalAlignment::Center;
        }
        const auto placement = PlacePopup(request);
        if (placement) {
            popup.popup_placement = *placement;
            Place(snapshot, id, placement->bounds);
        }
    }
}

void LayoutEngine::ComputeSubtree(SceneSnapshot &snapshot, contracts::NodeId root,
                                  contracts::LogicalRect bounds, const ShapeText &shaper)
{
    if (!root || !shaper || !std::isfinite(bounds.x) || !std::isfinite(bounds.y) ||
        !std::isfinite(bounds.width) || !std::isfinite(bounds.height) || bounds.width <= 0 ||
        bounds.height <= 0) {
        throw std::invalid_argument("Popup local layout requires finite positive bounds");
    }

    Measure(snapshot, root, shaper);
    Place(snapshot, root, bounds);
}
} // namespace prism::runtime
