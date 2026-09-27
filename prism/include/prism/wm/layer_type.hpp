#pragma once

#include <cstdint>
#include <string>

namespace prism::wm {

/**
 * @brief Window Manager Shell Layer Hierarchy
 *
 * Layer 1 (Desktop) : Wallpaper canvas, background widgets (Z-0) [Singleton]
 * Layer 2 (TopBar)  : Global header, status bar, system clock (Z-300) [Singleton]
 * Layer 3 (Dock)    : Application dock, quick launcher (Z-200) [Singleton]
 * Layer 4 (AppGroup): Tiling BSP root container & workspace manager (Z-100) [Singleton Root]
 * Layer 0 (App)     : Standard application window hosted inside an AppGroup workspace
 */
enum class LayerType : uint8_t { App = 0, Desktop = 1, TopBar = 2, Dock = 3, AppGroup = 4 };

inline const char *LayerTypeToString(LayerType type)
{
    switch (type) {
    case LayerType::Desktop:
        return "Desktop";
    case LayerType::TopBar:
        return "TopBar";
    case LayerType::Dock:
        return "Dock";
    case LayerType::AppGroup:
        return "AppGroup";
    case LayerType::App:
    default:
        return "App";
    }
}

inline LayerType StringToLayerType(const std::string &str)
{
    if (str == "desktop" || str == "Desktop") {
        return LayerType::Desktop;
    }
    if (str == "topbar" || str == "TopBar" || str == "panel" || str == "header") {
        return LayerType::TopBar;
    }
    if (str == "dock" || str == "Dock") {
        return LayerType::Dock;
    }
    if (str == "appgroup" || str == "AppGroup" || str == "workspace") {
        return LayerType::AppGroup;
    }
    return LayerType::App;
}

} // namespace prism::wm
