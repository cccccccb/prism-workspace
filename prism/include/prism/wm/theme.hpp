#pragma once

#include "prism/core/types.hpp"
#include "prism/contracts/theme_tokens.hpp"
#include <algorithm>

namespace prism::wm {
// Logical pixel geometry shared by Shell placement and tree work area. The
// surface bands include material/shadow margins; panels live inside the bands.
struct ThemeGeometry {
    int topbar_surface_height{static_cast<int>(contracts::theme::kShellTopHeight)};
    int topbar_panel_height{static_cast<int>(contracts::theme::kTopbarHeight)};
    int shell_margin{static_cast<int>(contracts::theme::kTopbarInset)};
    int dock_surface_height{static_cast<int>(contracts::theme::kShellBottomHeight)};
    int dock_panel_height{static_cast<int>(contracts::theme::kDockHeight)};
    int dock_bottom_margin{static_cast<int>(contracts::theme::kDockBottomInset)};
    int dock_max_width{static_cast<int>(contracts::theme::kDockWidth)};
    int inner_gap{static_cast<int>(contracts::theme::kInnerGap)};
    int outer_gap{static_cast<int>(contracts::theme::kOuterGap)};
    float window_radius{contracts::theme::kWindowRadius};
    float shadow_radius{contracts::theme::kShadowRadius};
    float shadow_opacity{contracts::theme::kShadow.a / 255.0f};
    float focused_border{contracts::theme::kWindowBorderWidth};
    float idle_border{contracts::theme::kWindowBorderWidth};

    core::Rect ShellRect(int role, int width, int height) const {
        if (role == 1) return {0, 0, static_cast<float>(width), static_cast<float>(height)};
        if (role == 2) return {0, 0, static_cast<float>(width), static_cast<float>(std::min(height, topbar_surface_height))};
        const auto w = std::max(1, std::min(dock_max_width, width));
        const auto h = std::max(1, std::min(dock_surface_height, height));
        return {static_cast<float>((width-w)/2), static_cast<float>(height-h), static_cast<float>(w), static_cast<float>(h)};
    }
    core::Rect WorkArea(int width, int height) const {
        const auto top = std::min(topbar_surface_height, height);
        return {0, static_cast<float>(top), static_cast<float>(width),
                static_cast<float>(std::max(1, height-top-dock_surface_height))};
    }
};
}
