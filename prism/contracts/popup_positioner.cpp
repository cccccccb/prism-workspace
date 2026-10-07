#include "prism/contracts/popup_positioner.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace prism::contracts {
namespace {

constexpr double MaximumDimension = 8192;
constexpr double MaximumGap = 256;

bool IsProtocolInteger(double value) noexcept
{
    return std::isfinite(value) && std::trunc(value) == value &&
           value >= std::numeric_limits<std::int32_t>::min() &&
           value <= std::numeric_limits<std::int32_t>::max();
}

bool IsPositiveDimension(double value) noexcept
{
    return std::isfinite(value) && value > 0 && value <= MaximumDimension;
}

bool IsParentGeometry(const LogicalRect &geometry) noexcept
{
    return IsProtocolInteger(geometry.x) && IsProtocolInteger(geometry.y) &&
           IsProtocolInteger(geometry.width) && IsProtocolInteger(geometry.height) &&
           IsPositiveDimension(geometry.width) && IsPositiveDimension(geometry.height) &&
           IsProtocolInteger(geometry.x + geometry.width) &&
           IsProtocolInteger(geometry.y + geometry.height);
}

bool IsAnchor(const LogicalRect &anchor) noexcept
{
    return std::isfinite(anchor.x) && std::isfinite(anchor.y) && std::isfinite(anchor.width) &&
           std::isfinite(anchor.height) && anchor.width > 0 && anchor.height > 0 &&
           std::isfinite(anchor.x + anchor.width) && std::isfinite(anchor.y + anchor.height);
}

bool IsAlignment(PopupHorizontalAlignment alignment) noexcept
{
    switch (alignment) {
    case PopupHorizontalAlignment::Start:
    case PopupHorizontalAlignment::Center:
    case PopupHorizontalAlignment::End:
        return true;
    }
    return false;
}

bool IsPreference(PopupVerticalPreference preference) noexcept
{
    return preference == PopupVerticalPreference::Below ||
           preference == PopupVerticalPreference::Above;
}

} // namespace

std::optional<PopupPositioner> PreparePopupPositioner(const PopupPositionerRequest &request,
                                                      const LogicalRect &parent_geometry) noexcept
{
    if (!IsParentGeometry(parent_geometry) || !IsAnchor(request.anchor) ||
        !IsPositiveDimension(request.desired.width) ||
        !IsPositiveDimension(request.desired.height) || !std::isfinite(request.gap) ||
        request.gap < 0 || request.gap > MaximumGap || !IsAlignment(request.horizontal_alignment) ||
        !IsPreference(request.vertical_preference)) {
        return std::nullopt;
    }

    const auto &anchor = request.anchor;
    const double left = std::max(anchor.x, parent_geometry.x);
    const double top = std::max(anchor.y, parent_geometry.y);
    const double right =
        std::min(anchor.x + anchor.width, parent_geometry.x + parent_geometry.width);
    const double bottom =
        std::min(anchor.y + anchor.height, parent_geometry.y + parent_geometry.height);
    if (left >= right || top >= bottom) {
        return std::nullopt;
    }

    const auto x = static_cast<std::int32_t>(std::floor(left - parent_geometry.x));
    const auto y = static_cast<std::int32_t>(std::floor(top - parent_geometry.y));
    const auto end_x = static_cast<std::int32_t>(std::ceil(right - parent_geometry.x));
    const auto end_y = static_cast<std::int32_t>(std::ceil(bottom - parent_geometry.y));
    if (end_x <= x || end_y <= y) {
        return std::nullopt;
    }

    return PopupPositioner{{x, y, end_x - x, end_y - y},
                           static_cast<std::int32_t>(std::ceil(request.desired.width)),
                           static_cast<std::int32_t>(std::ceil(request.desired.height)),
                           static_cast<std::int32_t>(std::ceil(request.gap)),
                           request.horizontal_alignment,
                           request.vertical_preference};
}

} // namespace prism::contracts
