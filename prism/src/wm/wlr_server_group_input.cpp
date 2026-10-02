#include "wlr_server_internal.hpp"

namespace prism::wm {
namespace {
// Mouse recovery policy, in output logical pixels. Not an animation parameter.
constexpr double RecoveryWidth = 160;
constexpr double RecoveryHeight = 6;
constexpr std::uint32_t PrimaryButton = 272;

bool Contains(const core::Rect &bounds, double x, double y)
{
    return x >= bounds.x && y >= bounds.y && x < bounds.x + bounds.width &&
           y < bounds.y + bounds.height;
}
} // namespace

bool WlrServer::GroupControlsHidden() const
{
    return GroupImmersive() ||
           std::any_of(xdg_views_.begin(), xdg_views_.end(), [](const auto &view) {
               return view->mapped && view->visible && view->fullscreen && !view->shell_role;
           });
}

WlrXdgView *WlrServer::RecoveryTopbar() const
{
    for (const auto &view : xdg_views_) {
        if (view->shell_role != static_cast<int>(contracts::WindowRole::TopBar) || !view->mapped) {
            continue;
        }
        const auto registration = registrations_.find(view->pid);
        if (registration != registrations_.end() &&
            registration->second->permit.instance.value == view->instance &&
            LayoutPrincipal(registration->second->permit).mapped) {
            return view.get();
        }
    }
    return nullptr;
}

bool WlrServer::ConsumeGroupPointer(std::uint32_t button, std::uint32_t state,
                                    wlr_input_device *device)
{
    const auto key = std::make_pair(device, button);
    if (!recovery_buttons_.empty()) {
        if (state == WLR_BUTTON_PRESSED) {
            recovery_buttons_.insert(key);
        } else {
            recovery_buttons_.erase(key);
        }
        // Motion and focus resume on the next event, after this complete chord.
        return true;
    }
    if (button != PrimaryButton || state != WLR_BUTTON_PRESSED || outputs_.size() != 1 ||
        !outputs_.front()->wlr_output->enabled || !GroupControlsHidden() ||
        seat_->pointer_state.button_count || dragged_xdg_view_ ||
        wlr_seat_pointer_has_grab(seat_)) {
        return false;
    }

    const auto output = PrimaryLogicalBounds();
    const auto width = std::min(RecoveryWidth, double(output.width));
    const core::Rect edge{float(output.x + (output.width - width) / 2), output.y, float(width),
                          float(std::min(RecoveryHeight, double(output.height)))};
    if (!Contains(edge, cursor_->x, cursor_->y)) {
        return false;
    }

    recovery_buttons_.insert(key);
    layout_controls_.CancelInput(contracts::LayoutInputKind::Pointer, 0);
    wlr_seat_pointer_notify_clear_focus(seat_);
    wlr_seat_pointer_notify_frame(seat_);
    if (RecoveryTopbar()) {
        recovery_visible_ = true;
        ArrangeXdgViews();
    } else {
        RestoreDesktopGroup();
    }
    return true;
}

void WlrServer::UpdateGroupRecovery()
{
    if (!recovery_visible_ || !recovery_buttons_.empty() || seat_->pointer_state.button_count) {
        return;
    }
    const auto output = PrimaryLogicalBounds();
    auto bounds = theme_.ShellRect(static_cast<int>(contracts::WindowRole::TopBar),
                                   int(output.width), int(output.height));
    bounds.x += output.x;
    bounds.y += output.y;
    if (GroupControlsHidden() && Contains(bounds, cursor_->x, cursor_->y)) {
        return;
    }
    if (const auto *topbar = RecoveryTopbar();
        topbar && layout_controls_.HasPendingInput({topbar->instance}, launch::MonotonicNs())) {
        return;
    }

    recovery_visible_ = false;
    ArrangeXdgViews();
}

bool WlrServer::ConstrainRecoveryFocus(wlr_surface *surface, double x, double y) const
{
    if (!recovery_visible_) {
        return true;
    }
    auto *root = wlr_surface_get_root_surface(surface);
    for (const auto &view : xdg_views_) {
        if (view->toplevel->base->surface == root &&
            view->shell_role == static_cast<int>(contracts::WindowRole::TopBar)) {
            return x >= view->x && y >= view->y && x < view->x + view->width &&
                   y < view->y + view->height;
        }
    }
    return true;
}

} // namespace prism::wm
