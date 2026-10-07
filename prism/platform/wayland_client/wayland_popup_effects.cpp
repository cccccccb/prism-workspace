#include "prism-surface-effects-client.h"
#include "prism/platform/wayland_popup.hpp"
#include "prism/platform/wayland_window.hpp"

#include <utility>
#include <wayland-client.h>

namespace prism::platform {
WaylandSurfaceEffectCapabilities WaylandPopup::SurfaceEffectCapabilities() const noexcept
{
    return !closed_ && parent_ ? parent_->SurfaceEffectCapabilities()
                               : WaylandSurfaceEffectCapabilities{};
}

bool WaylandPopup::SetSurfaceEffects(const WaylandPopupTarget &expected,
                                     std::span<const contracts::SurfaceEffectRegion> regions)
{
    if (!Matches(expected) || !buffer_layout_ || regions.size() > 8) {
        return false;
    }
    const auto capabilities = SurfaceEffectCapabilities();
    if (!regions.empty() && (!capabilities.backdrop || !capabilities.popup_backdrop)) {
        return false;
    }

    std::vector<contracts::SurfaceEffectRegion> next;
    std::vector<std::vector<std::uint8_t>> payloads;
    next.reserve(regions.size());
    payloads.reserve(regions.size());
    try {
        for (const auto &region : regions) {
            contracts::ValidateSurfaceEffectRegion(region);
            if (region.contour && !capabilities.contour) {
                return false;
            }
            auto payload = region.contour ? contracts::EncodeContour(*region.contour)
                                          : std::vector<std::uint8_t>{};
            auto transmitted = region;
            transmitted.blur_radius = wl_fixed_to_double(wl_fixed_from_double(region.blur_radius));
            if (!region.contour) {
                transmitted.bounds = {
                    wl_fixed_to_double(wl_fixed_from_double(region.bounds.x)),
                    wl_fixed_to_double(wl_fixed_from_double(region.bounds.y)),
                    wl_fixed_to_double(wl_fixed_from_double(region.bounds.width)),
                    wl_fixed_to_double(wl_fixed_from_double(region.bounds.height))};
                transmitted.corner_radius =
                    wl_fixed_to_double(wl_fixed_from_double(region.corner_radius));
            }
            contracts::ValidateSurfaceEffectRegion(transmitted);
            next.push_back(std::move(transmitted));
            payloads.push_back(std::move(payload));
        }
    } catch (...) {
        return false;
    }
    if (next == sent_effects_) {
        return true;
    }
    if (!surface_effect_) {
        if (!parent_->effect_manager_) {
            return false;
        }
        surface_effect_ =
            prism_surface_effect_manager_v1_get_surface_effect(parent_->effect_manager_, surface_);
        if (!surface_effect_) {
            return false;
        }
    }

    prism_surface_effect_v1_clear(surface_effect_);
    for (std::size_t index = 0; index < next.size(); ++index) {
        const auto &region = next[index];
        if (region.contour) {
            auto &payload = payloads[index];
            wl_array array{payload.size(), 0, payload.data()};
            prism_surface_effect_v1_add_contour(surface_effect_,
                                                wl_fixed_from_double(region.blur_radius), &array);
            continue;
        }
        prism_surface_effect_v1_add_region(
            surface_effect_, wl_fixed_from_double(region.bounds.x),
            wl_fixed_from_double(region.bounds.y), wl_fixed_from_double(region.bounds.width),
            wl_fixed_from_double(region.bounds.height), wl_fixed_from_double(region.corner_radius),
            wl_fixed_from_double(region.blur_radius));
    }

    sent_effects_ = std::move(next);
    state_pending_ = true;
    return true;
}

void WaylandPopup::ResetSurfaceEffects() noexcept
{
    if (surface_effect_) {
        prism_surface_effect_v1_clear(surface_effect_);
        state_pending_ = true;
    }
    sent_effects_.clear();
}

void WaylandPopup::CloseSurfaceEffects() noexcept
{
    if (surface_effect_) {
        prism_surface_effect_v1_destroy(surface_effect_);
    }
    surface_effect_ = nullptr;
    sent_effects_.clear();
}
} // namespace prism::platform
