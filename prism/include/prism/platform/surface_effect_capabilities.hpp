#pragma once

namespace prism::platform {
// Connection capabilities, negotiated before any role-specific descriptor is
// sent. Popup backdrop is advertised separately from root backdrop support.
struct WaylandSurfaceEffectCapabilities {
    bool backdrop{};
    bool contour{};
    bool popup_backdrop{};

    bool operator==(const WaylandSurfaceEffectCapabilities &) const = default;
};
} // namespace prism::platform
