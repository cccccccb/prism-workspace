#include "prism/platform/wayland_window.hpp"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

namespace prism::platform {

static std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Common Linux evdev keys mapped to USB HID usages. Unknown keys remain zero
// until a full keymap/IME adapter is added.
static std::uint32_t HidUsage(std::uint32_t key) {
    switch (key) {
        case KEY_A: return 0x04; case KEY_B: return 0x05;
        case KEY_C: return 0x06; case KEY_D: return 0x07;
        case KEY_E: return 0x08; case KEY_F: return 0x09;
        case KEY_G: return 0x0a; case KEY_H: return 0x0b;
        case KEY_I: return 0x0c; case KEY_J: return 0x0d;
        case KEY_K: return 0x0e; case KEY_L: return 0x0f;
        case KEY_M: return 0x10; case KEY_N: return 0x11;
        case KEY_O: return 0x12; case KEY_P: return 0x13;
        case KEY_Q: return 0x14; case KEY_R: return 0x15;
        case KEY_S: return 0x16; case KEY_T: return 0x17;
        case KEY_U: return 0x18; case KEY_V: return 0x19;
        case KEY_W: return 0x1a; case KEY_X: return 0x1b;
        case KEY_Y: return 0x1c; case KEY_Z: return 0x1d;
        case KEY_1: return 0x1e; case KEY_2: return 0x1f;
        case KEY_3: return 0x20; case KEY_4: return 0x21;
        case KEY_5: return 0x22; case KEY_6: return 0x23;
        case KEY_7: return 0x24; case KEY_8: return 0x25;
        case KEY_9: return 0x26; case KEY_0: return 0x27;
        case KEY_ENTER: return 0x28; case KEY_ESC: return 0x29;
        case KEY_BACKSPACE: return 0x2a; case KEY_TAB: return 0x2b;
        case KEY_SPACE: return 0x2c;
        case KEY_RIGHT: return 0x4f; case KEY_LEFT: return 0x50;
        case KEY_DOWN: return 0x51; case KEY_UP: return 0x52;
        default: return 0;
    }
}

struct WaylandWindow::ShmBuffer {
    wl_buffer* handle{nullptr};
    void* pixels{MAP_FAILED};
    std::size_t bytes{0};
    int width{0};
    int height{0};
    bool busy{false};

    ~ShmBuffer() {
        if (handle) wl_buffer_destroy(handle);
        if (pixels != MAP_FAILED) munmap(pixels, bytes);
    }
};

WaylandWindow::WaylandWindow() = default;
WaylandWindow::~WaylandWindow() { Close(); }

void WaylandWindow::Emit(contracts::WindowEvent event) {
    if (event_handler_) event_handler_(event);
}

void WaylandWindow::RegistryGlobal(void* data, wl_registry* registry,
                                   std::uint32_t name, const char* interface,
                                   std::uint32_t version) {
    auto& self = *static_cast<WaylandWindow*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        self.compositor_ = static_cast<wl_compositor*>(wl_registry_bind(
            registry, name, &wl_compositor_interface, std::min(version, 4u)));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
        self.shm_ = static_cast<wl_shm*>(wl_registry_bind(
            registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
        self.shell_ = static_cast<xdg_wm_base*>(wl_registry_bind(
            registry, name, &xdg_wm_base_interface, 1));
        static const xdg_wm_base_listener listener{.ping = ShellPing};
        xdg_wm_base_add_listener(self.shell_, &listener, &self);
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
        if (self.seat_) return; // One seat for the first client iteration.
        self.seat_ = static_cast<wl_seat*>(wl_registry_bind(
            registry, name, &wl_seat_interface, std::min(version, 5u)));
        self.seat_global_name_ = name;
        static const wl_seat_listener listener{.capabilities = SeatCapabilities,
                                                .name = SeatName};
        wl_seat_add_listener(self.seat_, &listener, &self);
    }
}

void WaylandWindow::RegistryGlobalRemove(void* data, wl_registry*, std::uint32_t name) {
    auto& self = *static_cast<WaylandWindow*>(data);
    if (name != self.seat_global_name_) return;
    if (self.pointer_) wl_pointer_release(self.pointer_);
    if (self.keyboard_) wl_keyboard_release(self.keyboard_);
    if (self.seat_) wl_seat_release(self.seat_);
    self.pointer_ = nullptr;
    self.keyboard_ = nullptr;
    self.seat_ = nullptr;
    self.seat_global_name_ = 0;
}
void WaylandWindow::ShellPing(void*, xdg_wm_base* shell, std::uint32_t serial) {
    xdg_wm_base_pong(shell, serial);
}

void WaylandWindow::SurfaceConfigure(void* data, xdg_surface* surface,
                                     std::uint32_t serial) {
    auto& self = *static_cast<WaylandWindow*>(data);
    xdg_surface_ack_configure(surface, serial);
    const int width = self.pending_width_ > 0 ? self.pending_width_
        : (self.configured_ ? static_cast<int>(self.metrics_.logical_size.width) : self.preferred_width_);
    const int height = self.pending_height_ > 0 ? self.pending_height_
        : (self.configured_ ? static_cast<int>(self.metrics_.logical_size.height) : self.preferred_height_);
    const int safe_width = std::clamp(width, 1, 4096);
    const int safe_height = std::clamp(height, 1, 4096);
    self.metrics_ = {{static_cast<double>(safe_width), static_cast<double>(safe_height)},
                     {static_cast<std::uint32_t>(safe_width), static_cast<std::uint32_t>(safe_height)}, 1.0};
    self.configured_ = true;
    ++self.configure_count_;
    self.dirty_ = true;
    self.Emit(contracts::ConfigureEvent{contracts::WindowId{1}, self.metrics_});
    if (self.frame_callback_) {
        wl_callback_destroy(self.frame_callback_);
        self.frame_callback_ = nullptr;
    }
    self.TryRender();
}

void WaylandWindow::ToplevelConfigure(void* data, xdg_toplevel*, std::int32_t width,
                                      std::int32_t height, wl_array*) {
    auto& self = *static_cast<WaylandWindow*>(data);
    self.pending_width_ = width;
    self.pending_height_ = height;
}
void WaylandWindow::ToplevelClose(void* data, xdg_toplevel*) {
    auto& self = *static_cast<WaylandWindow*>(data);
    self.close_requested_ = true;
    self.Emit(contracts::CloseRequestedEvent{contracts::WindowId{1}});
}
void WaylandWindow::ToplevelConfigureBounds(void*, xdg_toplevel*, std::int32_t, std::int32_t) {}
void WaylandWindow::ToplevelCapabilities(void*, xdg_toplevel*, wl_array*) {}

void WaylandWindow::SeatCapabilities(void* data, wl_seat* seat, std::uint32_t caps) {
    auto& self = *static_cast<WaylandWindow*>(data);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !self.pointer_) {
        self.pointer_ = wl_seat_get_pointer(seat);
        static const wl_pointer_listener listener{
            .enter = PointerEnter, .leave = PointerLeave, .motion = PointerMotion,
            .button = PointerButton, .axis = PointerAxis, .frame = PointerFrame,
            .axis_source = PointerAxisSource, .axis_stop = PointerAxisStop,
            .axis_discrete = PointerAxisDiscrete, .axis_value120 = PointerAxisValue120,
            .axis_relative_direction = PointerAxisRelativeDirection};
        wl_pointer_add_listener(self.pointer_, &listener, &self);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && self.pointer_) {
        wl_pointer_release(self.pointer_);
        self.pointer_ = nullptr;
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !self.keyboard_) {
        self.keyboard_ = wl_seat_get_keyboard(seat);
        static const wl_keyboard_listener listener{
            .keymap = KeyboardKeymap, .enter = KeyboardEnter, .leave = KeyboardLeave,
            .key = KeyboardKey, .modifiers = KeyboardModifiers,
            .repeat_info = KeyboardRepeatInfo};
        wl_keyboard_add_listener(self.keyboard_, &listener, &self);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && self.keyboard_) {
        wl_keyboard_release(self.keyboard_);
        self.keyboard_ = nullptr;
    }
}
void WaylandWindow::SeatName(void*, wl_seat*, const char*) {}
void WaylandWindow::PointerEnter(void* data, wl_pointer*, std::uint32_t,
                                 wl_surface*, wl_fixed_t x, wl_fixed_t y) {
    auto& self = *static_cast<WaylandWindow*>(data);
    ++self.pointer_enter_count_;
    self.pointer_position_ = {wl_fixed_to_double(x), wl_fixed_to_double(y)};
    self.Emit(contracts::PointerMotionEvent{contracts::WindowId{1},
                                            self.pointer_position_, NowNs()});
}
void WaylandWindow::PointerLeave(void*, wl_pointer*, std::uint32_t, wl_surface*) {}
void WaylandWindow::PointerMotion(void* data, wl_pointer*, std::uint32_t,
                                  wl_fixed_t x, wl_fixed_t y) {
    auto& self = *static_cast<WaylandWindow*>(data);
    self.pointer_position_ = {wl_fixed_to_double(x), wl_fixed_to_double(y)};
    self.Emit(contracts::PointerMotionEvent{contracts::WindowId{1},
                                            self.pointer_position_, NowNs()});
}
void WaylandWindow::PointerButton(void* data, wl_pointer*, std::uint32_t,
                                  std::uint32_t, std::uint32_t button,
                                  std::uint32_t state) {
    auto& self = *static_cast<WaylandWindow*>(data);
    ++self.pointer_button_count_;
    contracts::PointerButton mapped = contracts::PointerButton::Other;
    if (button == BTN_LEFT) mapped = contracts::PointerButton::Primary;
    else if (button == BTN_RIGHT) mapped = contracts::PointerButton::Secondary;
    else if (button == BTN_MIDDLE) mapped = contracts::PointerButton::Middle;
    else if (button == BTN_SIDE) mapped = contracts::PointerButton::Back;
    else if (button == BTN_EXTRA) mapped = contracts::PointerButton::Forward;
    self.Emit(contracts::PointerButtonEvent{contracts::WindowId{1},
        self.pointer_position_, mapped,
        state == WL_POINTER_BUTTON_STATE_PRESSED ? contracts::ButtonState::Pressed
                                                 : contracts::ButtonState::Released,
        mapped == contracts::PointerButton::Other ? button : 0, NowNs()});
}
void WaylandWindow::PointerAxis(void* data, wl_pointer*, std::uint32_t,
                                std::uint32_t axis, wl_fixed_t value) {
    auto& self = *static_cast<WaylandWindow*>(data);
    const double delta = wl_fixed_to_double(value);
    self.Emit(contracts::PointerScrollEvent{contracts::WindowId{1},
        self.pointer_position_, axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? delta : 0.0,
        axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? delta : 0.0, NowNs()});
}
void WaylandWindow::PointerFrame(void*, wl_pointer*) {}
void WaylandWindow::PointerAxisSource(void*, wl_pointer*, std::uint32_t) {}
void WaylandWindow::PointerAxisStop(void*, wl_pointer*, std::uint32_t, std::uint32_t) {}
void WaylandWindow::PointerAxisDiscrete(void*, wl_pointer*, std::uint32_t, std::int32_t) {}
void WaylandWindow::PointerAxisValue120(void*, wl_pointer*, std::uint32_t, std::int32_t) {}
void WaylandWindow::PointerAxisRelativeDirection(void*, wl_pointer*, std::uint32_t, std::uint32_t) {}
void WaylandWindow::KeyboardKeymap(void*, wl_keyboard*, std::uint32_t, int fd, std::uint32_t) {
    if (fd >= 0) close(fd);
}
void WaylandWindow::KeyboardEnter(void* data, wl_keyboard*, std::uint32_t,
                                  wl_surface*, wl_array*) {
    static_cast<WaylandWindow*>(data)->Emit(
        contracts::FocusEvent{contracts::WindowId{1}, true});
}
void WaylandWindow::KeyboardLeave(void* data, wl_keyboard*, std::uint32_t,
                                  wl_surface*) {
    static_cast<WaylandWindow*>(data)->Emit(
        contracts::FocusEvent{contracts::WindowId{1}, false});
}
void WaylandWindow::KeyboardKey(void* data, wl_keyboard*, std::uint32_t,
                                std::uint32_t, std::uint32_t key,
                                std::uint32_t state) {
    auto& self = *static_cast<WaylandWindow*>(data);
    ++self.key_count_;
    self.Emit(contracts::KeyEvent{contracts::WindowId{1}, HidUsage(key),
        state == WL_KEYBOARD_KEY_STATE_PRESSED ? contracts::ButtonState::Pressed
                                               : contracts::ButtonState::Released,
        false, NowNs()});
}
void WaylandWindow::KeyboardModifiers(void*, wl_keyboard*, std::uint32_t,
                                      std::uint32_t, std::uint32_t, std::uint32_t,
                                      std::uint32_t) {}
void WaylandWindow::KeyboardRepeatInfo(void*, wl_keyboard*, std::int32_t, std::int32_t) {}

void WaylandWindow::FrameDone(void* data, wl_callback* callback, std::uint32_t) {
    auto& self = *static_cast<WaylandWindow*>(data);
    wl_callback_destroy(callback);
    if (self.frame_callback_ == callback) self.frame_callback_ = nullptr;
    ++self.frame_done_count_;
    self.TryRender();
}
void WaylandWindow::BufferRelease(void* data, wl_buffer*) {
    auto* buffer = static_cast<ShmBuffer*>(data);
    buffer->busy = false;
}

WaylandWindow::ShmBuffer* WaylandWindow::AcquireBuffer() {
    const int width = static_cast<int>(metrics_.buffer_size.width);
    const int height = static_cast<int>(metrics_.buffer_size.height);
    for (auto& buffer : buffers_) {
        if (!buffer->busy && buffer->width == width && buffer->height == height)
            return buffer.get();
    }
    if (buffers_.size() >= 3) return nullptr;
    const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;
    int fd = memfd_create("prism-wayland-probe", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        if (fd >= 0) close(fd);
        return nullptr;
    }
    void* pixels = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    wl_shm_pool* pool = wl_shm_create_pool(shm_, fd, static_cast<int>(bytes));
    wl_buffer* handle = pool ? wl_shm_pool_create_buffer(pool, 0, width, height,
                                                        width * 4, WL_SHM_FORMAT_XRGB8888) : nullptr;
    if (pool) wl_shm_pool_destroy(pool);
    close(fd);
    if (!handle) {
        munmap(pixels, bytes);
        return nullptr;
    }
    auto buffer = std::make_unique<ShmBuffer>();
    buffer->handle = handle;
    buffer->pixels = pixels;
    buffer->bytes = bytes;
    buffer->width = width;
    buffer->height = height;
    static const wl_buffer_listener listener{.release = BufferRelease};
    wl_buffer_add_listener(handle, &listener, buffer.get());
    buffers_.push_back(std::move(buffer));
    return buffers_.back().get();
}

void WaylandWindow::ReapBuffers() {
    const int width = static_cast<int>(metrics_.buffer_size.width);
    const int height = static_cast<int>(metrics_.buffer_size.height);
    std::erase_if(buffers_, [=](const auto& buffer) {
        return !buffer->busy && (buffer->width != width || buffer->height != height);
    });
}

void WaylandWindow::TryRender() {
    if (!configured_ || !dirty_ || frame_callback_ || !surface_ ||
        (!paint_handler_ && !present_handler_)) return;
    if (present_handler_) {
        frame_callback_ = wl_surface_frame(surface_);
        static const wl_callback_listener listener{.done = FrameDone};
        wl_callback_add_listener(frame_callback_, &listener, this);
        const bool presented = present_handler_(display_, surface_,
            static_cast<int>(metrics_.buffer_size.width),
            static_cast<int>(metrics_.buffer_size.height));
        if (!presented) {
            wl_callback_destroy(frame_callback_);
            frame_callback_ = nullptr;
            return;
        }
        mapped_ = true;
        dirty_ = false;
        return;
    }
    ReapBuffers();
    ShmBuffer* buffer = AcquireBuffer();
    if (!buffer) return;
    paint_handler_(buffer->pixels, buffer->width, buffer->height, buffer->width * 4);
    buffer->busy = true;
    wl_surface_attach(surface_, buffer->handle, 0, 0);
    wl_surface_damage_buffer(surface_, 0, 0, buffer->width, buffer->height);
    frame_callback_ = wl_surface_frame(surface_);
    static const wl_callback_listener listener{.done = FrameDone};
    wl_callback_add_listener(frame_callback_, &listener, this);
    wl_surface_commit(surface_);
    mapped_ = true;
    dirty_ = false;
}

void WaylandWindow::RequestRedraw() {
    dirty_ = true;
    TryRender();
}

bool WaylandWindow::Open(const std::string& socket_name, const std::string& app_id,
                         const std::string& title, int preferred_width,
                         int preferred_height) {
    if (display_) return false;
    preferred_width_ = std::clamp(preferred_width, 1, 4096);
    preferred_height_ = std::clamp(preferred_height, 1, 4096);
    display_ = wl_display_connect(socket_name.empty() ? nullptr : socket_name.c_str());
    if (!display_) return false;
    registry_ = wl_display_get_registry(display_);
    static const wl_registry_listener registry_listener{
        .global = RegistryGlobal, .global_remove = RegistryGlobalRemove};
    wl_registry_add_listener(registry_, &registry_listener, this);
    if (wl_display_roundtrip(display_) < 0 || wl_display_roundtrip(display_) < 0 ||
        !compositor_ || !shm_ || !shell_) {
        Close();
        return false;
    }
    surface_ = wl_compositor_create_surface(compositor_);
    if (surface_) xdg_surface_ = xdg_wm_base_get_xdg_surface(shell_, surface_);
    if (xdg_surface_) toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
    if (!surface_ || !xdg_surface_ || !toplevel_) {
        Close();
        return false;
    }
    static const xdg_surface_listener surface_listener{.configure = SurfaceConfigure};
    static const xdg_toplevel_listener toplevel_listener{
        .configure = ToplevelConfigure, .close = ToplevelClose,
        .configure_bounds = ToplevelConfigureBounds,
        .wm_capabilities = ToplevelCapabilities};
    xdg_surface_add_listener(xdg_surface_, &surface_listener, this);
    xdg_toplevel_add_listener(toplevel_, &toplevel_listener, this);
    xdg_toplevel_set_app_id(toplevel_, app_id.c_str());
    xdg_toplevel_set_title(toplevel_, title.c_str());
    wl_surface_commit(surface_); // Required empty initial commit before any buffer.
    return wl_display_flush(display_) >= 0 || errno == EAGAIN;
}

bool WaylandWindow::Pump(int timeout_ms) {
    if (!display_) return false;
    while (wl_display_prepare_read(display_) != 0) {
        if (wl_display_dispatch_pending(display_) < 0) return false;
    }
    const int flushed = wl_display_flush(display_);
    if (flushed < 0 && errno != EAGAIN) {
        wl_display_cancel_read(display_);
        return false;
    }
    pollfd pfd{wl_display_get_fd(display_),
               static_cast<short>(POLLIN | (flushed < 0 ? POLLOUT : 0)), 0};
    const int result = poll(&pfd, 1, timeout_ms);
    if (result > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        wl_display_cancel_read(display_);
        return false;
    }
    if (result > 0 && (pfd.revents & POLLOUT)) wl_display_flush(display_);
    if (result > 0 && (pfd.revents & POLLIN)) {
        if (wl_display_read_events(display_) < 0) return false;
    } else {
        wl_display_cancel_read(display_);
        if (result < 0 && errno != EINTR) return false;
    }
    if (wl_display_dispatch_pending(display_) < 0) return false;
    ReapBuffers();
    if (dirty_ && !frame_callback_) TryRender();
    return !close_requested_;
}

void WaylandWindow::RequestMaximize() {
    if (toplevel_) xdg_toplevel_set_maximized(toplevel_);
}

void WaylandWindow::Close() {
    if (frame_callback_) wl_callback_destroy(frame_callback_);
    frame_callback_ = nullptr;
    buffers_.clear();
    if (toplevel_) xdg_toplevel_destroy(toplevel_);
    if (xdg_surface_) xdg_surface_destroy(xdg_surface_);
    if (surface_) wl_surface_destroy(surface_);
    if (pointer_) wl_pointer_release(pointer_);
    if (keyboard_) wl_keyboard_release(keyboard_);
    if (seat_) wl_seat_release(seat_);
    if (shell_) xdg_wm_base_destroy(shell_);
    if (shm_) wl_shm_destroy(shm_);
    if (compositor_) wl_compositor_destroy(compositor_);
    if (registry_) wl_registry_destroy(registry_);
    if (display_) wl_display_disconnect(display_);
    display_ = nullptr;
    registry_ = nullptr;
    compositor_ = nullptr;
    shm_ = nullptr;
    seat_ = nullptr;
    seat_global_name_ = 0;
    pointer_ = nullptr;
    keyboard_ = nullptr;
    shell_ = nullptr;
    surface_ = nullptr;
    xdg_surface_ = nullptr;
    toplevel_ = nullptr;
    configured_ = false;
    mapped_ = false;
}

} // namespace prism::platform
