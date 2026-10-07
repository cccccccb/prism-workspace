#include "prism/runtime/popup.hpp"

#include <algorithm>
#include <cmath>

namespace prism::runtime {
namespace {
bool Positive(double value)
{
    return std::isfinite(value) && value > 0;
}

bool Nonnegative(double value)
{
    return std::isfinite(value) && value >= 0;
}

bool Valid(contracts::LogicalRect rect)
{
    return std::isfinite(rect.x) && std::isfinite(rect.y) && Positive(rect.width) &&
           Positive(rect.height) && std::isfinite(rect.x + rect.width) &&
           std::isfinite(rect.y + rect.height);
}
} // namespace

std::optional<PopupPlacement> PlacePopup(const PopupPlacementRequest &request) noexcept
{
    if (!Valid(request.available) || !Valid(request.anchor) || !Positive(request.desired.width) ||
        !Positive(request.desired.height) || !Positive(request.minimum_width) ||
        !Positive(request.minimum_height) || request.minimum_width > request.desired.width ||
        request.minimum_height > request.desired.height || !Nonnegative(request.margin) ||
        !Nonnegative(request.gap) ||
        (request.horizontal_alignment != PopupHorizontalAlignment::Start &&
         request.horizontal_alignment != PopupHorizontalAlignment::Center)) {
        return std::nullopt;
    }

    const auto &area = request.available;
    contracts::LogicalRect anchor{std::max(area.x, request.anchor.x),
                                  std::max(area.y, request.anchor.y), 0, 0};
    anchor.width = std::max(
        0.0, std::min(area.x + area.width, request.anchor.x + request.anchor.width) - anchor.x);
    anchor.height = std::max(
        0.0, std::min(area.y + area.height, request.anchor.y + request.anchor.height) - anchor.y);
    if (anchor.width <= 0 || anchor.height <= 0) {
        return std::nullopt;
    }
    const double width = area.width - 2 * request.margin;
    const double height = area.height - 2 * request.margin;
    if (!Positive(width) || !Positive(height)) {
        return std::nullopt;
    }
    const double left = area.x + request.margin;
    const double top = area.y + request.margin;
    const double right = left + width;
    const double bottom = top + height;
    // An offscreen anchor is unavailable; do not leave an orphan floating panel.
    if (anchor.x >= right || anchor.x + anchor.width <= left || anchor.y >= bottom ||
        anchor.y + anchor.height <= top) {
        return std::nullopt;
    }

    const double below_start = std::min(bottom, anchor.y + anchor.height + request.gap);
    const double above_end = std::max(top, anchor.y - request.gap);
    const double below = std::max(0.0, bottom - below_start);
    const double above = std::max(0.0, above_end - top);
    PopupPlacement result;
    result.effective_anchor = anchor;
    result.bounds.width = std::min(width, request.desired.width);
    result.bounds.height = std::min(height, request.desired.height);
    const double desired_x = request.horizontal_alignment == PopupHorizontalAlignment::Center
                                 ? anchor.x + anchor.width / 2 - result.bounds.width / 2
                                 : anchor.x;
    result.bounds.x = std::clamp(desired_x, left, right - result.bounds.width);

    if (width < request.minimum_width || std::max(above, below) < request.minimum_height) {
        result.side = PopupSide::EdgePanel;
        result.bounds.x = left;
        result.bounds.width = width;
        result.bounds.y = bottom - result.bounds.height;
    } else {
        const bool use_below =
            below >= request.desired.height || (above < request.desired.height && below >= above);
        result.side = use_below ? PopupSide::Below : PopupSide::Above;
        result.bounds.height = std::min(request.desired.height, use_below ? below : above);
        result.bounds.y = use_below ? below_start : above_end - result.bounds.height;
    }
    result.width_constrained = result.bounds.width < request.desired.width;
    result.height_constrained = result.bounds.height < request.desired.height;

    return result;
}
} // namespace prism::runtime
