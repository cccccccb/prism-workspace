#pragma once

#include "prism/contracts/types.hpp"

#include <cstdint>
#include <optional>

namespace prism::contracts {

enum class PopupHorizontalAlignment { Start, Center, End };
enum class PopupVerticalPreference { Below, Above };

struct PopupPositionerRequest {
    LogicalRect anchor;  // Parent surface logical coordinates, before window geometry subtraction.
    LogicalSize desired; // Entire popup window geometry, including any functional attachment.
    double gap{8};
    PopupHorizontalAlignment horizontal_alignment{PopupHorizontalAlignment::Center};
    PopupVerticalPreference vertical_preference{PopupVerticalPreference::Below};
};

struct PopupPositionerRect {
    std::int32_t x{}, y{}, width{}, height{};
    bool operator==(const PopupPositionerRect &) const noexcept = default;
};

struct PopupPositioner {
    PopupPositionerRect anchor; // Parent window geometry coordinates, ready for the wire boundary.
    std::int32_t width{}, height{}, gap{};
    PopupHorizontalAlignment horizontal_alignment{PopupHorizontalAlignment::Center};
    PopupVerticalPreference vertical_preference{PopupVerticalPreference::Below};
    bool operator==(const PopupPositioner &) const noexcept = default;
};

// parent_geometry is the trusted, actually committed integer window geometry
// in parent surface coordinates, not an application-supplied output rectangle.
// This prepares preferences only; the compositor's configure is authoritative.
std::optional<PopupPositioner> PreparePopupPositioner(const PopupPositionerRequest &request,
                                                      const LogicalRect &parent_geometry) noexcept;

} // namespace prism::contracts
