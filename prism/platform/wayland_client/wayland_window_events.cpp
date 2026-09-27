#include "presentation-time-client-protocol.h"
#include "prism-surface-effects-client.h"
#include "wayland_window_p.hpp"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

namespace prism::platform {
static std::uint64_t NowNs()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// Common Linux evdev keys mapped to USB HID usages. Unknown keys remain zero
// until a full keymap/IME adapter is added.
static std::uint32_t HidUsage(std::uint32_t key)
{
    switch (key) {
    case KEY_A:
        return 0x04;
    case KEY_B:
        return 0x05;
    case KEY_C:
        return 0x06;
    case KEY_D:
        return 0x07;
    case KEY_E:
        return 0x08;
    case KEY_F:
        return 0x09;
    case KEY_G:
        return 0x0a;
    case KEY_H:
        return 0x0b;
    case KEY_I:
        return 0x0c;
    case KEY_J:
        return 0x0d;
    case KEY_K:
        return 0x0e;
    case KEY_L:
        return 0x0f;
    case KEY_M:
        return 0x10;
    case KEY_N:
        return 0x11;
    case KEY_O:
        return 0x12;
    case KEY_P:
        return 0x13;
    case KEY_Q:
        return 0x14;
    case KEY_R:
        return 0x15;
    case KEY_S:
        return 0x16;
    case KEY_T:
        return 0x17;
    case KEY_U:
        return 0x18;
    case KEY_V:
        return 0x19;
    case KEY_W:
        return 0x1a;
    case KEY_X:
        return 0x1b;
    case KEY_Y:
        return 0x1c;
    case KEY_Z:
        return 0x1d;
    case KEY_1:
        return 0x1e;
    case KEY_2:
        return 0x1f;
    case KEY_3:
        return 0x20;
    case KEY_4:
        return 0x21;
    case KEY_5:
        return 0x22;
    case KEY_6:
        return 0x23;
    case KEY_7:
        return 0x24;
    case KEY_8:
        return 0x25;
    case KEY_9:
        return 0x26;
    case KEY_0:
        return 0x27;
    case KEY_ENTER:
        return 0x28;
    case KEY_ESC:
        return 0x29;
    case KEY_BACKSPACE:
        return 0x2a;
    case KEY_TAB:
        return 0x2b;
    case KEY_SPACE:
        return 0x2c;
    case KEY_RIGHT:
        return 0x4f;
    case KEY_LEFT:
        return 0x50;
    case KEY_DOWN:
        return 0x51;
    case KEY_UP:
        return 0x52;
    default:
        return 0;
    }
}

void WaylandWindow::RegistryGlobal(void *data, wl_registry *registry, std::uint32_t name,
                                   const char *interface, std::uint32_t version)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        self.compositor_ = static_cast<wl_compositor *>(
            wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4u)));
    } else if (std::strcmp(interface, wp_presentation_interface.name) == 0) {
        self.presentation_ = static_cast<wp_presentation *>(
            wl_registry_bind(registry, name, &wp_presentation_interface, 1));
        static const wp_presentation_listener listener{.clock_id = PresentationClock};
        wp_presentation_add_listener(self.presentation_, &listener, &self);
    } else if (std::strcmp(interface, prism_surface_effect_manager_v1_interface.name) == 0) {
        self.effect_manager_ = static_cast<prism_surface_effect_manager_v1 *>(
            wl_registry_bind(registry, name, &prism_surface_effect_manager_v1_interface, 1));
        static const prism_surface_effect_manager_v1_listener listener{.capabilities =
                                                                           EffectCapabilities};
        prism_surface_effect_manager_v1_add_listener(self.effect_manager_, &listener, &self);
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
        self.shm_ = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
        self.shell_ =
            static_cast<xdg_wm_base *>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
        static const xdg_wm_base_listener listener{.ping = ShellPing};
        xdg_wm_base_add_listener(self.shell_, &listener, &self);
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
        if (self.seat_) {
            return; // One seat for the first client iteration.
        }
        self.seat_ = static_cast<wl_seat *>(
            wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
        self.seat_global_name_ = name;
        static const wl_seat_listener listener{.capabilities = SeatCapabilities, .name = SeatName};
        wl_seat_add_listener(self.seat_, &listener, &self);
    }
}

void WaylandWindow::EffectCapabilities(void *data, prism_surface_effect_manager_v1 *,
                                       std::uint32_t supported)
{
    static_cast<WaylandWindow *>(data)->backdrop_supported_ = supported != 0;
}

void WaylandWindow::RegistryGlobalRemove(void *data, wl_registry *, std::uint32_t name)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    if (name != self.seat_global_name_) {
        return;
    }
    if (self.pointer_) {
        wl_pointer_release(self.pointer_);
    }
    if (self.keyboard_) {
        wl_keyboard_release(self.keyboard_);
    }
    if (self.seat_) {
        wl_seat_release(self.seat_);
    }
    self.pointer_ = nullptr;
    self.keyboard_ = nullptr;
    self.seat_ = nullptr;
    self.seat_global_name_ = 0;
}

void WaylandWindow::ShellPing(void *, xdg_wm_base *shell, std::uint32_t serial)
{
    xdg_wm_base_pong(shell, serial);
}

void WaylandWindow::SurfaceConfigure(void *data, xdg_surface *surface, std::uint32_t serial)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    xdg_surface_ack_configure(surface, serial);
    const int width = self.pending_width_ > 0
                          ? self.pending_width_
                          : (self.configured_ ? static_cast<int>(self.metrics_.logical_size.width)
                                              : self.preferred_width_);
    const int height = self.pending_height_ > 0
                           ? self.pending_height_
                           : (self.configured_ ? static_cast<int>(self.metrics_.logical_size.height)
                                               : self.preferred_height_);
    const int safe_width = std::clamp(width, 1, 4096);
    const int safe_height = std::clamp(height, 1, 4096);
    const bool resize = !self.mapped_ ||
                        self.metrics_.buffer_size.width != static_cast<std::uint32_t>(safe_width) ||
                        self.metrics_.buffer_size.height != static_cast<std::uint32_t>(safe_height);
    self.metrics_ = {
        {static_cast<double>(safe_width), static_cast<double>(safe_height)},
        {static_cast<std::uint32_t>(safe_width), static_cast<std::uint32_t>(safe_height)},
        1.0};
    self.configured_ = true;
    ++self.configure_count_;
    self.update_requested_ = true;
    self.state_pending_ = true; // The configure acknowledgement requires a commit.
    self.force_pixels_ |= resize;
    if (resize) {
        self.input_sent_ = false;
    }
    self.Emit(contracts::ConfigureEvent{contracts::WindowId{1}, self.metrics_});
    if (resize && self.frame_callback_) {
        wl_callback_destroy(self.frame_callback_);
        self.frame_callback_ = nullptr;
    }
    self.TrySubmit();
}

void WaylandWindow::ToplevelConfigure(void *data, xdg_toplevel *, std::int32_t width,
                                      std::int32_t height, wl_array *)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    self.pending_width_ = width;
    self.pending_height_ = height;
}

void WaylandWindow::ToplevelClose(void *data, xdg_toplevel *)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    self.close_requested_ = true;
    self.Emit(contracts::CloseRequestedEvent{contracts::WindowId{1}});
}

void WaylandWindow::ToplevelConfigureBounds(void *, xdg_toplevel *, std::int32_t, std::int32_t)
{
}

void WaylandWindow::ToplevelCapabilities(void *, xdg_toplevel *, wl_array *)
{
}

void WaylandWindow::SeatCapabilities(void *data, wl_seat *seat, std::uint32_t caps)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !self.pointer_) {
        self.pointer_ = wl_seat_get_pointer(seat);
        static const wl_pointer_listener listener{.enter = PointerEnter,
                                                  .leave = PointerLeave,
                                                  .motion = PointerMotion,
                                                  .button = PointerButton,
                                                  .axis = PointerAxis,
                                                  .frame = PointerFrame,
                                                  .axis_source = PointerAxisSource,
                                                  .axis_stop = PointerAxisStop,
                                                  .axis_discrete = PointerAxisDiscrete,
                                                  .axis_value120 = PointerAxisValue120,
                                                  .axis_relative_direction =
                                                      PointerAxisRelativeDirection};
        wl_pointer_add_listener(self.pointer_, &listener, &self);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && self.pointer_) {
        wl_pointer_release(self.pointer_);
        self.pointer_ = nullptr;
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !self.keyboard_) {
        self.keyboard_ = wl_seat_get_keyboard(seat);
        static const wl_keyboard_listener listener{.keymap = KeyboardKeymap,
                                                   .enter = KeyboardEnter,
                                                   .leave = KeyboardLeave,
                                                   .key = KeyboardKey,
                                                   .modifiers = KeyboardModifiers,
                                                   .repeat_info = KeyboardRepeatInfo};
        wl_keyboard_add_listener(self.keyboard_, &listener, &self);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && self.keyboard_) {
        wl_keyboard_release(self.keyboard_);
        self.keyboard_ = nullptr;
    }
}

void WaylandWindow::SeatName(void *, wl_seat *, const char *)
{
}

void WaylandWindow::PointerEnter(void *data, wl_pointer *, std::uint32_t, wl_surface *,
                                 wl_fixed_t x, wl_fixed_t y)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    ++self.pointer_enter_count_;
    self.pointer_position_ = {wl_fixed_to_double(x), wl_fixed_to_double(y)};
    self.Emit(
        contracts::PointerMotionEvent{contracts::WindowId{1}, self.pointer_position_, NowNs()});
}

void WaylandWindow::PointerLeave(void *data, wl_pointer *, std::uint32_t, wl_surface *)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    self.pointer_position_ = {-1, -1};
    self.Emit(
        contracts::PointerMotionEvent{contracts::WindowId{1}, self.pointer_position_, NowNs()});
}

void WaylandWindow::PointerMotion(void *data, wl_pointer *, std::uint32_t, wl_fixed_t x,
                                  wl_fixed_t y)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    self.pointer_position_ = {wl_fixed_to_double(x), wl_fixed_to_double(y)};
    self.Emit(
        contracts::PointerMotionEvent{contracts::WindowId{1}, self.pointer_position_, NowNs()});
}

void WaylandWindow::PointerButton(void *data, wl_pointer *, std::uint32_t, std::uint32_t,
                                  std::uint32_t button, std::uint32_t state)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    ++self.pointer_button_count_;
    contracts::PointerButton mapped = contracts::PointerButton::Other;
    if (button == BTN_LEFT) {
        mapped = contracts::PointerButton::Primary;
    } else if (button == BTN_RIGHT) {
        mapped = contracts::PointerButton::Secondary;
    } else if (button == BTN_MIDDLE) {
        mapped = contracts::PointerButton::Middle;
    } else if (button == BTN_SIDE) {
        mapped = contracts::PointerButton::Back;
    } else if (button == BTN_EXTRA) {
        mapped = contracts::PointerButton::Forward;
    }
    self.Emit(contracts::PointerButtonEvent{
        contracts::WindowId{1}, self.pointer_position_, mapped,
        state == WL_POINTER_BUTTON_STATE_PRESSED ? contracts::ButtonState::Pressed
                                                 : contracts::ButtonState::Released,
        mapped == contracts::PointerButton::Other ? button : 0, NowNs()});
}

void WaylandWindow::PointerAxis(void *data, wl_pointer *, std::uint32_t, std::uint32_t axis,
                                wl_fixed_t value)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    const double delta = wl_fixed_to_double(value);
    self.Emit(contracts::PointerScrollEvent{contracts::WindowId{1}, self.pointer_position_,
                                            axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? delta : 0.0,
                                            axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? delta : 0.0,
                                            NowNs()});
}

void WaylandWindow::PointerFrame(void *, wl_pointer *)
{
}

void WaylandWindow::PointerAxisSource(void *, wl_pointer *, std::uint32_t)
{
}

void WaylandWindow::PointerAxisStop(void *, wl_pointer *, std::uint32_t, std::uint32_t)
{
}

void WaylandWindow::PointerAxisDiscrete(void *, wl_pointer *, std::uint32_t, std::int32_t)
{
}

void WaylandWindow::PointerAxisValue120(void *, wl_pointer *, std::uint32_t, std::int32_t)
{
}

void WaylandWindow::PointerAxisRelativeDirection(void *, wl_pointer *, std::uint32_t, std::uint32_t)
{
}

void WaylandWindow::KeyboardKeymap(void *, wl_keyboard *, std::uint32_t, int fd, std::uint32_t)
{
    if (fd >= 0) {
        close(fd);
    }
}

void WaylandWindow::KeyboardEnter(void *data, wl_keyboard *, std::uint32_t, wl_surface *,
                                  wl_array *)
{
    static_cast<WaylandWindow *>(data)->Emit(contracts::FocusEvent{contracts::WindowId{1}, true});
}

void WaylandWindow::KeyboardLeave(void *data, wl_keyboard *, std::uint32_t, wl_surface *)
{
    static_cast<WaylandWindow *>(data)->Emit(contracts::FocusEvent{contracts::WindowId{1}, false});
}

void WaylandWindow::KeyboardKey(void *data, wl_keyboard *, std::uint32_t, std::uint32_t,
                                std::uint32_t key, std::uint32_t state)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    ++self.key_count_;
    self.Emit(contracts::KeyEvent{contracts::WindowId{1}, HidUsage(key),
                                  state == WL_KEYBOARD_KEY_STATE_PRESSED
                                      ? contracts::ButtonState::Pressed
                                      : contracts::ButtonState::Released,
                                  false, NowNs()});
}

void WaylandWindow::KeyboardModifiers(void *, wl_keyboard *, std::uint32_t, std::uint32_t,
                                      std::uint32_t, std::uint32_t, std::uint32_t)
{
}

void WaylandWindow::KeyboardRepeatInfo(void *, wl_keyboard *, std::int32_t, std::int32_t)
{
}

void WaylandWindow::FrameDone(void *data, wl_callback *callback, std::uint32_t)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    wl_callback_destroy(callback);
    if (self.frame_callback_ == callback) {
        self.frame_callback_ = nullptr;
    }
    ++self.frame_done_count_;
    self.TrySubmit();
}

void WaylandWindow::BufferRelease(void *data, wl_buffer *)
{
    auto *buffer = static_cast<ShmBuffer *>(data);
    buffer->busy = false;
}

void WaylandWindow::PresentationClock(void *, wp_presentation *, std::uint32_t)
{
}

void WaylandWindow::PresentationOutput(void *, struct wp_presentation_feedback *, wl_output *)
{
}

void WaylandWindow::FinishPresentation(struct wp_presentation_feedback *feedback)
{
    std::erase(feedbacks_, feedback);
    wp_presentation_feedback_destroy(feedback);
}

void WaylandWindow::PresentationDone(void *data, struct wp_presentation_feedback *feedback,
                                     std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                     std::uint32_t, std::uint32_t, std::uint32_t)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    ++self.presentation_count_;
    self.FinishPresentation(feedback);
}

void WaylandWindow::PresentationDiscarded(void *data, struct wp_presentation_feedback *feedback)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    ++self.discarded_count_;
    self.FinishPresentation(feedback);
    // A discarded startup frame must not leave a static UI without another attempt.
    if (!self.presentation_count_) {
        self.update_requested_ = true;
        self.force_pixels_ = true;
        // This callback belongs to an already committed, discarded frame.
        // Its client proxy may be retired so startup can submit a replacement.
        if (self.frame_callback_) {
            wl_callback_destroy(self.frame_callback_);
        }
        self.frame_callback_ = nullptr;
    }
}

} // namespace prism::platform
