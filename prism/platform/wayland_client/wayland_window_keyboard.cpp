#include "prism/platform/wayland_window.hpp"

#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

namespace prism::platform {
namespace {
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

bool ModifierActive(xkb_state *state, const char *name)
{
    return state && xkb_state_mod_name_is_active(state, name, XKB_STATE_MODS_EFFECTIVE) > 0;
}
} // namespace

contracts::KeyModifiers WaylandWindow::CurrentKeyModifiers() const
{
    return {ModifierActive(keyboard_state_, XKB_MOD_NAME_SHIFT),
            ModifierActive(keyboard_state_, XKB_MOD_NAME_CTRL),
            ModifierActive(keyboard_state_, XKB_MOD_NAME_ALT),
            ModifierActive(keyboard_state_, XKB_MOD_NAME_LOGO)};
}

void WaylandWindow::ReleaseKeyboard()
{
    const auto source = KeyboardSource();
    const bool notify = keyboard_ != nullptr;
    auto *focus = keyboard_focus_surface_;
    pending_keyboard_focus_.clear();
    if (keyboard_) {
        wl_keyboard_release(keyboard_);
        keyboard_ = nullptr;
    }

    xkb_state_unref(keyboard_state_);
    keyboard_state_ = nullptr;
    xkb_context_unref(keyboard_context_);
    keyboard_context_ = nullptr;
    keyboard_focus_surface_ = nullptr;

    if (notify) {
        const contracts::FocusEvent event{contracts::WindowId{1}, false, source};
        if (FindPopup(focus)) {
            EmitSurfaceInput(focus, event);
        } else {
            Emit(event);
        }
    }
}

void WaylandWindow::KeyboardKeymap(void *data, wl_keyboard *, std::uint32_t format, int fd,
                                   std::uint32_t size)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    xkb_state_unref(self.keyboard_state_);
    self.keyboard_state_ = nullptr;
    if (fd < 0) {
        return;
    }

    // The wire format is a NUL-terminated string. Bound both the file and
    // allocation before allowing libxkbcommon to inspect untrusted bytes.
    struct stat metadata{};
    const bool valid = format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 0 &&
                       size <= 16 * 1024 * 1024 && fstat(fd, &metadata) == 0 &&
                       metadata.st_size >= static_cast<off_t>(size);
    void *mapping = valid ? mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    close(fd);
    if (mapping == MAP_FAILED) {
        return;
    }

    if (!self.keyboard_context_) {
        self.keyboard_context_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    }
    const auto *text = static_cast<const char *>(mapping);
    xkb_keymap *keymap{};
    if (self.keyboard_context_ && text[size - 1] == '\0') {
        keymap = xkb_keymap_new_from_string(self.keyboard_context_, text, XKB_KEYMAP_FORMAT_TEXT_V1,
                                            XKB_KEYMAP_COMPILE_NO_FLAGS);
    }
    munmap(mapping, size);
    if (keymap) {
        self.keyboard_state_ = xkb_state_new(keymap);
        xkb_keymap_unref(keymap);
    }
}

void WaylandWindow::KeyboardEnter(void *data, wl_keyboard *, std::uint32_t, wl_surface *surface,
                                  wl_array *)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    self.keyboard_focus_surface_ = surface;
    self.pending_keyboard_focus_.push_back({surface, true, self.KeyboardSource()});
}

void WaylandWindow::KeyboardLeave(void *data, wl_keyboard *, std::uint32_t, wl_surface *surface)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    if (self.keyboard_focus_surface_ == surface) {
        self.keyboard_focus_surface_ = nullptr;
    }
    self.pending_keyboard_focus_.push_back({surface, false, self.KeyboardSource()});
    if (self.keyboard_state_) {
        xkb_state_update_mask(self.keyboard_state_, 0, 0, 0, 0, 0, 0);
    }
}

void WaylandWindow::KeyboardKey(void *data, wl_keyboard *, std::uint32_t, std::uint32_t,
                                std::uint32_t key, std::uint32_t state)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    if (!self.keyboard_focus_surface_) {
        return;
    }
    self.FlushKeyboardFocus();
    auto *focus = self.keyboard_focus_surface_;
    if (!focus) {
        return;
    }
    if (focus == self.surface_) {
        ++self.key_count_;
    }
    self.EmitSurfaceInput(focus, contracts::KeyEvent{contracts::WindowId{1}, HidUsage(key),
                                                     state == WL_KEYBOARD_KEY_STATE_PRESSED
                                                         ? contracts::ButtonState::Pressed
                                                         : contracts::ButtonState::Released,
                                                     false, InputTimeNs(), self.KeyboardSource(),
                                                     self.CurrentKeyModifiers()});
    const auto modifiers = self.CurrentKeyModifiers();
    if (focus == self.keyboard_focus_surface_ && state == WL_KEYBOARD_KEY_STATE_PRESSED &&
        self.keyboard_state_ && !modifiers.control && !modifiers.alt && !modifiers.meta) {
        char text[128]{};
        const int length =
            xkb_state_key_get_utf8(self.keyboard_state_, key + 8, text, sizeof(text));
        if (length > 0 && length < static_cast<int>(sizeof(text)) &&
            static_cast<unsigned char>(text[0]) >= 32 && text[0] != 127) {
            self.EmitSurfaceInput(
                focus, contracts::TextInputEvent{contracts::WindowId{1}, std::string(text, length),
                                                 InputTimeNs(), self.KeyboardSource()});
        }
    }
}

void WaylandWindow::KeyboardModifiers(void *data, wl_keyboard *, std::uint32_t,
                                      std::uint32_t depressed, std::uint32_t latched,
                                      std::uint32_t locked, std::uint32_t group)
{
    auto &self = *static_cast<WaylandWindow *>(data);
    if (self.keyboard_state_) {
        xkb_state_update_mask(self.keyboard_state_, depressed, latched, locked, 0, 0, group);
    }
}

void WaylandWindow::KeyboardRepeatInfo(void *, wl_keyboard *, std::int32_t, std::int32_t)
{
    // This adapter does not synthesize repeat events yet. Activation state
    // still rejects repeat events supplied by another adapter.
}

} // namespace prism::platform
