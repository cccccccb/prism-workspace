#pragma once

#include "prism/wm/window.hpp"
#include "prism/wm/layer_type.hpp"
#include "prism/core/types.hpp"
#include <memory>
#include <vector>

namespace prism::wm {

enum class LayerRegisterResult {
    Success,
    AlreadyExists,    // Singleton layer already has an active client
    InvalidLayer
};

/**
 * @brief Manages the 4 fundamental Window Manager Shell Layers:
 *        1. Desktop (Canvas/Wallpaper) - [Strict Singleton]
 *        2. TopBar  (Status/Title Bar) - [Strict Singleton]
 *        3. Dock    (App Launcher)     - [Strict Singleton]
 *        4. AppGroup(BSL/BSP Tiling Root Group) - [Strict Singleton]
 *
 * All clients can still be declared, styled, and decorated using Prism DSL.
 */
class LayerManager {
public:
    LayerManager();
    ~LayerManager() = default;

    // Registers a window to a layer. Fails with AlreadyExists if a singleton is already occupied.
    LayerRegisterResult RegisterWindow(const std::shared_ptr<Window>& win, LayerType layer);

    // Unregisters a window, releasing its singleton lease if occupied.
    void UnregisterWindow(const std::shared_ptr<Window>& win);

    // Checks whether a specific singleton layer is currently occupied.
    bool IsLayerOccupied(LayerType layer) const;

    // Calculates the usable workspace area for the AppGroup (BSP tree root)
    // by deducting TopBar and Dock exclusive margins from screen dimensions.
    core::Rect CalculateUsableArea(int screen_w, int screen_h) const;


    // Getters
    std::shared_ptr<Window> GetDesktop() const { return desktop_window_; }
    std::shared_ptr<Window> GetTopBar() const { return topbar_window_; }
    std::shared_ptr<Window> GetDock() const { return dock_window_; }
    std::shared_ptr<Window> GetAppGroup() const { return app_group_window_; }
    bool HasAppGroup() const { return has_app_group_; }

private:
    std::shared_ptr<Window> desktop_window_{nullptr};
    std::shared_ptr<Window> topbar_window_{nullptr};
    std::shared_ptr<Window> dock_window_{nullptr};
    std::shared_ptr<Window> app_group_window_{nullptr};
    bool has_app_group_{false};
};

} // namespace prism::wm
