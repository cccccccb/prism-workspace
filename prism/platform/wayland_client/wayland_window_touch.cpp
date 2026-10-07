#include "prism/platform/wayland_window.hpp"

#include <algorithm>

namespace prism::platform {

contracts::InputSource WaylandWindow::TouchSource() const
{
    return {seat_identity_, 3, touch_generation_};
}

void WaylandWindow::UpdateTouchCapability(wl_seat *seat, std::uint32_t caps)
{
    if (!(caps & WL_SEAT_CAPABILITY_TOUCH)) {
        ReleaseTouch();
        return;
    }
    if (touch_) {
        return;
    }

    touch_ = wl_seat_get_touch(seat);
    ++touch_generation_;
    static const wl_touch_listener listener{.down = TouchDown,
                                            .up = TouchUp,
                                            .motion = TouchMotion,
                                            .frame = TouchFrame,
                                            .cancel = TouchCancel,
                                            .shape = TouchShape,
                                            .orientation = TouchOrientation};
    wl_touch_add_listener(touch_, &listener, this);
}

void WaylandWindow::ReleaseTouch()
{
    if (!touch_) {
        return;
    }

    const auto source = TouchSource();
    wl_touch_release(touch_);
    touch_ = nullptr;
    touch_contacts_.clear();
    touch_frame_pending_ = false;

    Emit(contracts::TouchCancelEvent{contracts::WindowId{1}, InputTimeNs(), source});
}

void WaylandWindow::TouchDown(void *data, wl_touch *, std::uint32_t serial, std::uint32_t,
                              wl_surface *surface, std::int32_t contact, wl_fixed_t x, wl_fixed_t y)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    if (surface != self.surface_ ||
        std::find(self.touch_contacts_.begin(), self.touch_contacts_.end(), contact) !=
            self.touch_contacts_.end()) {
        return;
    }

    self.touch_contacts_.push_back(contact);
    self.touch_frame_pending_ = true;
    self.Emit(contracts::TouchDownEvent{contracts::WindowId{1},
                                        {wl_fixed_to_double(x), wl_fixed_to_double(y)},
                                        contact,
                                        InputTimeNs(),
                                        self.TouchSource(),
                                        serial});
}

void WaylandWindow::TouchUp(void *data, wl_touch *, std::uint32_t, std::uint32_t,
                            std::int32_t contact)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    if (std::erase(self.touch_contacts_, contact) == 0) {
        return;
    }
    self.touch_frame_pending_ = true;

    self.Emit(contracts::TouchUpEvent{contracts::WindowId{1}, contact, InputTimeNs(),
                                      self.TouchSource()});
}

void WaylandWindow::TouchMotion(void *data, wl_touch *, std::uint32_t, std::int32_t contact,
                                wl_fixed_t x, wl_fixed_t y)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    if (std::find(self.touch_contacts_.begin(), self.touch_contacts_.end(), contact) ==
        self.touch_contacts_.end()) {
        return;
    }
    self.touch_frame_pending_ = true;

    self.Emit(contracts::TouchMotionEvent{contracts::WindowId{1},
                                          {wl_fixed_to_double(x), wl_fixed_to_double(y)},
                                          contact,
                                          InputTimeNs(),
                                          self.TouchSource()});
}

void WaylandWindow::TouchFrame(void *data, wl_touch *)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    if (!self.touch_frame_pending_) {
        return;
    }
    self.touch_frame_pending_ = false;
    self.Emit(
        contracts::TouchFrameEvent{contracts::WindowId{1}, InputTimeNs(), self.TouchSource()});
}

void WaylandWindow::TouchCancel(void *data, wl_touch *)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    if (self.touch_contacts_.empty() && !self.touch_frame_pending_) {
        return;
    }
    self.touch_contacts_.clear();
    self.touch_frame_pending_ = false;
    self.Emit(
        contracts::TouchCancelEvent{contracts::WindowId{1}, InputTimeNs(), self.TouchSource()});
}

void WaylandWindow::TouchShape(void *, wl_touch *, std::int32_t, wl_fixed_t, wl_fixed_t)
{
}

void WaylandWindow::TouchOrientation(void *, wl_touch *, std::int32_t, wl_fixed_t)
{
}

} // namespace prism::platform
