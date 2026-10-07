#include "prism/platform/wayland_popup.hpp"
#include "prism/platform/wayland_window.hpp"

#include <algorithm>
#include <utility>

namespace prism::platform {
WaylandPopup *WaylandWindow::FindPopup(wl_surface *surface) const noexcept
{
    if (!surface) {
        return nullptr;
    }
    const auto found = std::find_if(popups_.begin(), popups_.end(), [surface](WaylandPopup *popup) {
        return !popup->closed_ && popup->surface_ == surface;
    });
    return found == popups_.end() ? nullptr : *found;
}

void WaylandWindow::EmitSurfaceInput(wl_surface *surface, contracts::WindowEvent event)
{
    if (surface && surface == surface_) {
        Emit(std::move(event));
        return;
    }
    if (auto *popup = FindPopup(surface)) {
        popup->RouteInput(std::move(event));
    }
}

void WaylandWindow::FlushKeyboardFocus()
{
    const auto pending = std::exchange(pending_keyboard_focus_, {});
    const bool internal =
        keyboard_focus_surface_ &&
        (keyboard_focus_surface_ == surface_ || FindPopup(keyboard_focus_surface_) != nullptr);
    for (const auto &focus : pending) {
        EmitSurfaceInput(focus.surface,
                         contracts::FocusEvent{contracts::WindowId{1}, focus.focused, focus.source,
                                               !focus.focused && internal});
    }
}
} // namespace prism::platform
