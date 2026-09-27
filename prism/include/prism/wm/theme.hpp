#pragma once

#include "prism/contracts/theme.hpp"
#include "prism/core/types.hpp"
#include "prism/tree/tree_layout_config.hpp"
#include <algorithm>
#include <cmath>

namespace prism::wm {
// Resolved values only. Theme parsing and named token lookup belong to the
// launcher and SDK; bootstrap geometry has no reserved bands or decoration.
struct ThemeGeometry {
    contracts::ThemeLayout layout{};

    core::Rect ShellRect(int role, int width, int height) const
    {
        if (role == 1) {
            return {0, 0, float(width), float(height)};
        }
        if (role == 2) {
            return {0, 0, float(width),
                    float(std::clamp(int(std::round(layout.topbar_surface_height)), 1, height))};
        }
        const int w = std::clamp(int(std::round(layout.dock_max_width)), 1, width);
        const int h = std::clamp(int(std::round(layout.dock_surface_height)), 1, height);
        return {float((width - w) / 2), float(height - h), float(w), float(h)};
    }

    core::Rect WorkArea(int width, int height) const
    {
        const int top =
            std::clamp(int(std::round(layout.topbar_surface_height)), 0, std::max(0, height - 1));
        const int bottom = std::clamp(int(std::round(layout.dock_surface_height)), 0,
                                      std::max(0, height - top - 1));
        return {0, float(top), float(width), float(height - top - bottom)};
    }

    tree::TreeLayoutConfig TreeLayout() const
    {
        return {int(std::round(layout.inner_gap)), int(std::round(layout.outer_gap)), false, 0};
    }
};

struct DecorationState {
    std::uint64_t generation{};
    contracts::ThemeDecoration style{.enabled = false};
};

inline DecorationState ResolveDecoration(const contracts::ThemeSnapshot *theme, bool focused,
                                         bool fullscreen, bool shell)
{
    if (!theme) {
        return {};
    }
    if (shell) {
        return {theme->generation, {.enabled = false}};
    }
    return {theme->generation, fullscreen ? theme->fullscreen
                               : focused  ? theme->focused
                                          : theme->normal};
}
} // namespace prism::wm
