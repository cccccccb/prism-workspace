#include "prism/tree/tree_engine.hpp"
#include "prism/wm/window.hpp"
#include "tree_split_layout_p.hpp"
#include <cmath>
#include <limits>

namespace prism::tree {
namespace {

constexpr double kFractionFloor = 0.00010001;
constexpr double kGeometryTolerance = 0.0001;

struct BoundaryNodes {
    std::shared_ptr<ContainerNode> parent;
    std::shared_ptr<TreeNode> before;
    std::shared_ptr<TreeNode> after;
    std::size_t index{0};
    std::uint64_t id{0};

    explicit operator bool() const
    {
        return bool(parent);
    }

    bool Horizontal() const
    {
        return parent->GetLayoutMode() == LayoutMode::SplitHorizontal;
    }
};

BoundaryNodes FindBoundary(const std::shared_ptr<TreeNode> &node, std::uint64_t id)
{
    if (const auto container = std::dynamic_pointer_cast<ContainerNode>(node)) {
        const auto &boundaries = container->GetBoundaries();
        for (std::size_t i = 0; i < boundaries.size(); ++i) {
            if (boundaries[i].id == id) {
                const auto &children = container->GetChildren();
                return {container, children[i], children[i + 1], i, id};
            }
        }
    }

    for (const auto &child : node->GetChildren()) {
        if (const auto result = FindBoundary(child, id)) {
            return result;
        }
    }
    return {};
}

BoundaryNodes FindBoundary(const std::vector<std::shared_ptr<WorkspaceNode>> &workspaces,
                           std::uint64_t id)
{
    for (const auto &workspace : workspaces) {
        if (const auto result = FindBoundary(workspace, id)) {
            return result;
        }
    }
    return {};
}

int EffectiveGap(const std::shared_ptr<TreeNode> &node, int inner_gap)
{
    auto ancestor = node->GetParent();
    while (ancestor) {
        const auto container = std::dynamic_pointer_cast<ContainerNode>(ancestor);
        if (container && (container->GetLayoutMode() == LayoutMode::Tabbed ||
                          container->GetLayoutMode() == LayoutMode::Stacked)) {
            return 0;
        }
        ancestor = ancestor->GetParent();
    }
    return std::max(0, inner_gap);
}

TreeMinimumSize MinimumSize(const std::shared_ptr<TreeNode> &node, int gap, double header)
{
    if (!node) {
        return {};
    }
    if (const auto window = node->GetWindow()) {
        return {std::max(1.0, std::ceil(double(window->GetMinimumWidth()))),
                std::max(1.0, std::ceil(double(window->GetMinimumHeight())))};
    }

    const auto container = std::dynamic_pointer_cast<ContainerNode>(node);
    const auto mode = container ? container->GetLayoutMode() : LayoutMode::None;
    const bool horizontal = mode == LayoutMode::SplitHorizontal || mode == LayoutMode::None;
    const bool vertical = mode == LayoutMode::SplitVertical;
    const bool headers = mode == LayoutMode::Tabbed || mode == LayoutMode::Stacked;
    TreeMinimumSize result;
    for (const auto &child : node->GetChildren()) {
        const auto child_size = MinimumSize(child, headers ? 0 : gap, header);
        result.width =
            horizontal ? result.width + child_size.width : std::max(result.width, child_size.width);
        result.height = vertical ? result.height + child_size.height
                                 : std::max(result.height, child_size.height);
    }

    const auto count = node->GetChildren().size();
    if (count > 1 && horizontal) {
        result.width += double(count - 1) * gap;
    }
    if (count > 1 && vertical) {
        result.height += double(count - 1) * gap;
    }
    if (count > 0 && headers) {
        result.height += header * (mode == LayoutMode::Stacked ? double(count) : 1.0);
    }
    return result;
}

double AxisStart(const core::Rect &rect, bool horizontal)
{
    return horizontal ? rect.x : rect.y;
}

double AxisExtent(const core::Rect &rect, bool horizontal)
{
    return horizontal ? rect.width : rect.height;
}

double AxisFraction(const std::shared_ptr<TreeNode> &node, bool horizontal)
{
    return horizontal ? node->GetWidthFraction() : node->GetHeightFraction();
}

bool SetAxisFraction(const std::shared_ptr<TreeNode> &node, bool horizontal, double fraction)
{
    return horizontal ? node->SetFractions(fraction, node->GetHeightFraction())
                      : node->SetFractions(node->GetWidthFraction(), fraction);
}

bool Fits(const core::Rect &bounds, const TreeMinimumSize &minimum)
{
    return bounds.width + kGeometryTolerance >= minimum.width &&
           bounds.height + kGeometryTolerance >= minimum.height;
}

BoundaryFractions Fractions(const BoundaryNodes &nodes, std::uint64_t topology)
{
    return {nodes.id,
            nodes.parent->GetNodeId(),
            nodes.before->GetNodeId(),
            nodes.after->GetNodeId(),
            topology,
            nodes.parent->GetLayoutMode(),
            AxisFraction(nodes.before, nodes.Horizontal()),
            AxisFraction(nodes.after, nodes.Horizontal())};
}

bool SameIdentity(const BoundaryFractions &a, const BoundaryFractions &b)
{
    return a.boundary == b.boundary && a.parent == b.parent && a.before == b.before &&
           a.after == b.after && a.topology_revision == b.topology_revision && a.axis == b.axis;
}

bool ValidFractions(const BoundaryFractions &fractions)
{
    return std::isfinite(fractions.before_fraction) && std::isfinite(fractions.after_fraction) &&
           fractions.before_fraction > kFractionFloor &&
           fractions.after_fraction > kFractionFloor && fractions.before_fraction <= 1 &&
           fractions.after_fraction <= 1;
}

// Use the same rounding and tiny-area rules as placement. A candidate must preserve
// both pair outer edges and every unrelated sibling's geometry, not only its weight.
bool ValidateCandidate(const BoundaryNodes &nodes, const SplitLayoutMetrics &metrics, double before,
                       double after, const TreeMinimumSize &before_min,
                       const TreeMinimumSize &after_min)
{
    if (!std::isfinite(before) || !std::isfinite(after) || before <= kFractionFloor ||
        after <= kFractionFloor || before > 1 || after > 1) {
        return false;
    }

    const bool horizontal = nodes.Horizontal();
    const auto &parent_bounds = nodes.parent->GetBounds();
    const float origin = float(AxisStart(parent_bounds, horizontal));
    float cursor = origin;
    const auto &children = nodes.parent->GetChildren();
    for (std::size_t i = 0; i < children.size(); ++i) {
        double fraction = AxisFraction(children[i], horizontal);
        if (i == nodes.index) {
            fraction = before;
        } else if (i == nodes.index + 1) {
            fraction = after;
        }
        const float remaining = std::max(0.0f, origin + metrics.axis - cursor);
        const float extent = SplitExtent(metrics, remaining, int(i), fraction);
        const auto &old = children[i]->GetBounds();
        if (i == nodes.index || i == nodes.index + 1) {
            const auto &minimum = i == nodes.index ? before_min : after_min;
            if (extent + kGeometryTolerance < (horizontal ? minimum.width : minimum.height)) {
                return false;
            }
            if (i == nodes.index &&
                std::abs(cursor - AxisStart(old, horizontal)) > kGeometryTolerance) {
                return false;
            }
            if (i == nodes.index + 1 &&
                std::abs(cursor + extent - AxisStart(old, horizontal) -
                         AxisExtent(old, horizontal)) > kGeometryTolerance) {
                return false;
            }
        } else if (std::abs(extent - AxisExtent(old, horizontal)) > kGeometryTolerance ||
                   std::abs(cursor - AxisStart(old, horizontal)) > kGeometryTolerance) {
            return false;
        }
        cursor += extent + metrics.gap;
    }
    return true;
}

} // namespace

TreeMinimumSize TreeEngine::GetMinimumSize(const std::shared_ptr<TreeNode> &node,
                                           const TreeLayoutConfig &config) const
{
    if (!std::isfinite(config.header_height)) {
        const double invalid = std::numeric_limits<double>::infinity();
        return {invalid, invalid};
    }
    return MinimumSize(node, node ? EffectiveGap(node, config.inner_gap) : 0,
                       std::max(16.0, double(config.header_height)));
}

std::optional<BoundaryRange> TreeEngine::GetBoundaryRange(std::uint64_t boundary,
                                                          const TreeLayoutConfig &config) const
{
    const auto nodes = FindBoundary(workspaces_, boundary);
    if (!nodes) {
        return std::nullopt;
    }

    const bool horizontal = nodes.Horizontal();
    const auto &parent = nodes.parent->GetBounds();
    const auto &before = nodes.before->GetBounds();
    const auto &after = nodes.after->GetBounds();
    const double origin = AxisStart(parent, horizontal);
    const double start = AxisStart(before, horizontal) - origin;
    const double gap = AxisStart(after, horizontal) - AxisStart(before, horizontal) -
                       AxisExtent(before, horizontal);
    const double end = AxisStart(after, horizontal) - origin + AxisExtent(after, horizontal);
    const auto before_min = GetMinimumSize(nodes.before, config);
    const auto after_min = GetMinimumSize(nodes.after, config);
    const double before_axis = std::ceil(horizontal ? before_min.width : before_min.height);
    const double after_axis = std::ceil(horizontal ? after_min.width : after_min.height);
    BoundaryRange range;
    range.boundary = boundary;
    range.parent = nodes.parent->GetNodeId();
    range.workspace = nodes.parent->GetWorkspace()->GetNodeId();
    range.before = nodes.before->GetNodeId();
    range.after = nodes.after->GetNodeId();
    range.axis = nodes.parent->GetLayoutMode();
    range.position = start + AxisExtent(before, horizontal) + gap / 2;
    range.minimum = start + before_axis + gap / 2;
    range.maximum = start + std::floor(end - start - gap - after_axis) + gap / 2;
    range.pair_fraction =
        AxisFraction(nodes.before, horizontal) + AxisFraction(nodes.after, horizontal);
    const auto metrics =
        SplitMetrics(float(AxisExtent(parent, horizontal)), int(nodes.parent->GetChildren().size()),
                     EffectiveGap(nodes.parent, config.inner_gap));
    // Positive weights at or below the legacy normalization floor are treated as
    // unassigned. Exclude those values even for unusually large logical outputs.
    range.minimum = std::max(
        range.minimum,
        range.position +
            std::floor((kFractionFloor - AxisFraction(nodes.before, horizontal)) * metrics.usable) +
            1);
    range.maximum = std::min(
        range.maximum,
        range.position +
            std::ceil((AxisFraction(nodes.after, horizontal) - kFractionFloor) * metrics.usable) -
            1);
    range.feasible = gap >= 0 && std::isfinite(range.minimum) && std::isfinite(range.maximum) &&
                     range.minimum <= range.maximum &&
                     Fits(parent, GetMinimumSize(nodes.parent, config)) &&
                     (horizontal ? parent.height >= std::max(before_min.height, after_min.height)
                                 : parent.width >= std::max(before_min.width, after_min.width));

    double total = 0;
    for (const auto &child : nodes.parent->GetChildren()) {
        const double fraction = AxisFraction(child, horizontal);
        total += fraction;
        if (!std::isfinite(fraction) || fraction <= kFractionFloor ||
            (child != nodes.before && child != nodes.after &&
             !Fits(child->GetBounds(), GetMinimumSize(child, config)))) {
            range.feasible = false;
        }
    }
    range.feasible = range.feasible && std::abs(total - 1.0) <= 1e-12;
    return range;
}

std::optional<BoundaryFractions> TreeEngine::CaptureBoundaryFractions(std::uint64_t boundary) const
{
    const auto nodes = FindBoundary(workspaces_, boundary);
    return nodes ? std::optional(Fractions(nodes, revisions_->topology)) : std::nullopt;
}

bool TreeEngine::ApplyBoundary(std::uint64_t boundary, double position,
                               const TreeLayoutConfig &config)
{
    if (!std::isfinite(position)) {
        return false;
    }
    const auto range = GetBoundaryRange(boundary, config);
    const auto nodes = FindBoundary(workspaces_, boundary);
    if (!range || !range->feasible || !nodes) {
        return false;
    }

    const bool horizontal = nodes.Horizontal();
    const auto &parent = nodes.parent->GetBounds();
    const auto metrics =
        SplitMetrics(float(AxisExtent(parent, horizontal)), int(nodes.parent->GetChildren().size()),
                     EffectiveGap(nodes.parent, config.inner_gap));
    if (metrics.usable <= 0) {
        return false;
    }

    const double requested = std::clamp(position, range->minimum, range->maximum);
    const double delta = std::round(requested - range->position);
    const double pair = range->pair_fraction;
    const double current = AxisFraction(nodes.before, horizontal);
    double before = current + delta / metrics.usable;
    const auto before_min = GetMinimumSize(nodes.before, config);
    const auto after_min = GetMinimumSize(nodes.after, config);
    if (!ValidateCandidate(nodes, metrics, before, pair - before, before_min, after_min)) {
        // Rounding thresholds can lie exactly on a float representable boundary.
        // Choose the centre of the common interval for both requested pixel widths.
        const double width = AxisExtent(nodes.before->GetBounds(), horizontal) + delta;
        const double next_width = AxisExtent(nodes.after->GetBounds(), horizontal) - delta;
        double lower = std::max(kFractionFloor, (width - 0.5) / metrics.usable);
        double upper = std::min(pair - kFractionFloor, (width + 0.5) / metrics.usable);
        if (nodes.index + 2 < nodes.parent->GetChildren().size()) {
            lower = std::max(lower, pair - (next_width + 0.5) / metrics.usable);
            upper = std::min(upper, pair - (next_width - 0.5) / metrics.usable);
        }
        if (lower >= upper) {
            return false;
        }
        before = (lower + upper) / 2;
        if (!ValidateCandidate(nodes, metrics, before, pair - before, before_min, after_min)) {
            return false;
        }
    }

    SetAxisFraction(nodes.before, horizontal, before);
    SetAxisFraction(nodes.after, horizontal, pair - before);
    return true;
}

bool TreeEngine::RestoreBoundaryFractions(const BoundaryFractions &saved,
                                          const BoundaryFractions &expected_current)
{
    const auto nodes = FindBoundary(workspaces_, saved.boundary);
    if (!nodes || !ValidFractions(saved) || !ValidFractions(expected_current)) {
        return false;
    }
    const auto current = Fractions(nodes, revisions_->topology);
    if (!SameIdentity(current, saved) || !SameIdentity(current, expected_current) ||
        current.before_fraction != expected_current.before_fraction ||
        current.after_fraction != expected_current.after_fraction ||
        std::abs((saved.before_fraction + saved.after_fraction) -
                 (current.before_fraction + current.after_fraction)) > 1e-12) {
        return false;
    }

    SetAxisFraction(nodes.before, nodes.Horizontal(), saved.before_fraction);
    SetAxisFraction(nodes.after, nodes.Horizontal(), saved.after_fraction);
    return true;
}

} // namespace prism::tree
