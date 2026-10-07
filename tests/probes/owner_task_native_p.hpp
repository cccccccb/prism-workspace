#pragma once
#include "prism/ipc/wm_messages.hpp"
#include "prism/runtime/owner_file_panel.hpp"
#include "prism/runtime/owner_task_panel.hpp"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

namespace prism::tests::owner_task_probe {
using Point = prism::contracts::LogicalPoint;
using Rect = prism::contracts::LogicalRect;

void Require(bool condition, std::string_view detail)
{
    if (!condition) {
        throw std::runtime_error(std::string(detail));
    }
}

class Descriptor {
public:
    explicit Descriptor(int value) : value_(value)
    {
        Require(value_ >= 0, "Cannot create native probe descriptor");
    }

    ~Descriptor()
    {
        close(value_);
    }

    int Get() const
    {
        return value_;
    }

private:
    int value_;
};

class Input {
public:
    ~Input()
    {
        if (keyboard_) {
            zwp_virtual_keyboard_v1_destroy(keyboard_);
        }
        if (keyboard_manager_) {
            zwp_virtual_keyboard_manager_v1_destroy(keyboard_manager_);
        }
        if (pointer_) {
            zwlr_virtual_pointer_v1_destroy(pointer_);
        }
        if (pointer_manager_) {
            zwlr_virtual_pointer_manager_v1_destroy(pointer_manager_);
        }
        if (output_) {
            wl_output_destroy(output_);
        }
        if (seat_) {
            wl_seat_destroy(seat_);
        }
        if (registry_) {
            wl_registry_destroy(registry_);
        }
        if (display_) {
            wl_display_disconnect(display_);
        }
    }

    void Connect(const char *socket)
    {
        const auto *runtime = std::getenv("XDG_RUNTIME_DIR");
        Require(runtime && std::filesystem::path(runtime).filename().string().starts_with(
                               "prism-owner-task-"),
                "Owner-task input probe requires its isolated temporary runtime");
        Require(socket && *socket && std::string_view(socket).find('/') == std::string_view::npos,
                "Owner-task probe requires an isolated Wayland socket basename");
        display_ = wl_display_connect(socket);
        Require(display_, "Cannot connect native owner-task input");
        registry_ = wl_display_get_registry(display_);
        Require(registry_ && wl_registry_add_listener(registry_, &registry_listener_, this) == 0,
                "Cannot listen to native input registry");
        Roundtrip();
        Roundtrip();
        Require(pointer_manager_ && keyboard_manager_ && seat_ && output_ && width_ && height_ &&
                    scale_ == 1 && transform_ == WL_OUTPUT_TRANSFORM_NORMAL && outputs_ == 1,
                "Expected one normal scale-one output and both native virtual input protocols");

        pointer_ = zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
            pointer_manager_, seat_, output_);
        keyboard_ =
            zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(keyboard_manager_, seat_);
        Require(pointer_ && keyboard_, "Cannot create native pointer/keyboard");
        PrepareKeyboard();
        Roundtrip();
        Move({0, 0});
    }

    void Click(Point point)
    {
        Move(point);
        zwlr_virtual_pointer_v1_button(pointer_, ++time_, 272, WL_POINTER_BUTTON_STATE_PRESSED);
        zwlr_virtual_pointer_v1_frame(pointer_);
        Roundtrip();
        zwlr_virtual_pointer_v1_button(pointer_, ++time_, 272, WL_POINTER_BUTTON_STATE_RELEASED);
        zwlr_virtual_pointer_v1_frame(pointer_);
        Roundtrip();
    }

    void Escape()
    {
        zwp_virtual_keyboard_v1_modifiers(keyboard_, 0, 0, 0, 0);
        zwp_virtual_keyboard_v1_key(keyboard_, ++time_, 1, WL_KEYBOARD_KEY_STATE_PRESSED);
        zwp_virtual_keyboard_v1_key(keyboard_, ++time_, 1, WL_KEYBOARD_KEY_STATE_RELEASED);
        Roundtrip();
    }

    void SelectAll()
    {
        zwp_virtual_keyboard_v1_modifiers(keyboard_, control_modifier_, 0, 0, 0);
        Key(30);
        zwp_virtual_keyboard_v1_modifiers(keyboard_, 0, 0, 0, 0);
        Roundtrip();
    }

    void Type(char character)
    {
        std::uint32_t key{};
        switch (character) {
        case 'n':
            key = 49;
            break;
        case 'e':
            key = 18;
            break;
        case 'w':
            key = 17;
            break;
        case 'm':
            key = 50;
            break;
        case 'd':
            key = 32;
            break;
        case '.':
            key = 52;
            break;
        default:
            Require(false, "Native filename fixture has an unsupported key");
        }
        Key(key);
    }

private:
    void Key(std::uint32_t key)
    {
        zwp_virtual_keyboard_v1_key(keyboard_, ++time_, key, WL_KEYBOARD_KEY_STATE_PRESSED);
        zwp_virtual_keyboard_v1_key(keyboard_, ++time_, key, WL_KEYBOARD_KEY_STATE_RELEASED);
        Roundtrip();
    }

    void Move(Point point)
    {
        Require(std::isfinite(point.x) && std::isfinite(point.y) && point.x >= 0 && point.y >= 0 &&
                    point.x < width_ && point.y < height_,
                "Native input point is outside the isolated output");
        zwlr_virtual_pointer_v1_motion_absolute(pointer_, ++time_, std::llround(point.x),
                                                std::llround(point.y), width_, height_);
        zwlr_virtual_pointer_v1_frame(pointer_);
        Roundtrip();
    }

    void Roundtrip()
    {
        Require(wl_display_roundtrip(display_) >= 0, "Native input roundtrip failed");
    }

    void PrepareKeyboard()
    {
        auto *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        Require(context, "Cannot create native XKB context");
        xkb_rule_names names{};
        names.layout = "us";
        auto *keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
        xkb_context_unref(context);
        Require(keymap, "Cannot prepare native keymap");
        const auto control = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CTRL);
        Require(control != XKB_MOD_INVALID && control < 32, "Native keymap has no Control mask");
        control_modifier_ = 1u << control;
        std::unique_ptr<char, decltype(&std::free)> text(
            xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1), &std::free);
        xkb_keymap_unref(keymap);
        Require(bool(text), "Cannot serialize native keymap");

        const auto size = std::strlen(text.get()) + 1;
        Descriptor fd(memfd_create("prism-owner-task-keymap", MFD_CLOEXEC));
        std::size_t at = 0;
        while (at < size) {
            const auto written = write(fd.Get(), text.get() + at, size - at);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            Require(written > 0, "Cannot write native keymap");
            at += static_cast<std::size_t>(written);
        }
        zwp_virtual_keyboard_v1_keymap(keyboard_, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd.Get(),
                                       static_cast<std::uint32_t>(size));
        Roundtrip();
    }

    static void Global(void *data, wl_registry *registry, std::uint32_t name, const char *interface,
                       std::uint32_t version)
    {
        auto &self = *static_cast<Input *>(data);
        if (std::strcmp(interface, wl_seat_interface.name) == 0 && !self.seat_) {
            self.seat_ = static_cast<wl_seat *>(
                wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
        } else if (std::strcmp(interface, wl_output_interface.name) == 0) {
            ++self.outputs_;
            if (!self.output_) {
                self.output_ = static_cast<wl_output *>(
                    wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 2u)));
                Require(wl_output_add_listener(self.output_, &output_listener_, &self) == 0,
                        "Cannot listen to native output geometry");
            }
        } else if (std::strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0 &&
                   version >= 2) {
            self.pointer_manager_ = static_cast<zwlr_virtual_pointer_manager_v1 *>(
                wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 2));
        } else if (std::strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
            self.keyboard_manager_ = static_cast<zwp_virtual_keyboard_manager_v1 *>(
                wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1));
        }
    }

    static void Remove(void *, wl_registry *, std::uint32_t)
    {
    }

    static void Geometry(void *data, wl_output *, std::int32_t, std::int32_t, std::int32_t,
                         std::int32_t, std::int32_t, const char *, const char *,
                         std::int32_t transform)
    {
        static_cast<Input *>(data)->transform_ = transform;
    }

    static void Mode(void *data, wl_output *, std::uint32_t flags, std::int32_t width,
                     std::int32_t height, std::int32_t)
    {
        if (flags & WL_OUTPUT_MODE_CURRENT) {
            auto &self = *static_cast<Input *>(data);
            self.width_ = width;
            self.height_ = height;
        }
    }

    static void Done(void *, wl_output *)
    {
    }

    static void Scale(void *data, wl_output *, std::int32_t scale)
    {
        static_cast<Input *>(data)->scale_ = scale;
    }

    inline static const wl_registry_listener registry_listener_{Global, Remove};
    inline static const wl_output_listener output_listener_{Geometry, Mode, Done, Scale};
    wl_display *display_{};
    wl_registry *registry_{};
    wl_seat *seat_{};
    wl_output *output_{};
    zwlr_virtual_pointer_manager_v1 *pointer_manager_{};
    zwlr_virtual_pointer_v1 *pointer_{};
    zwp_virtual_keyboard_manager_v1 *keyboard_manager_{};
    zwp_virtual_keyboard_v1 *keyboard_{};
    std::uint32_t time_{1000}, width_{}, height_{}, outputs_{};
    std::uint32_t control_modifier_{};
    std::int32_t scale_{1}, transform_{};
};

prism::ipc::TreeMessage Tree()
{
    const auto *runtime = std::getenv("XDG_RUNTIME_DIR");
    Require(runtime, "Native tree query has no runtime directory");
    const auto path = (std::filesystem::path(runtime) / "prism-ipc.sock").string();
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    Require(path.size() < sizeof(address.sun_path), "Native IPC path is too long");
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    Descriptor fd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    const timeval timeout{1, 0};
    Require(setsockopt(fd.Get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
                setsockopt(fd.Get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0,
            "Cannot bound native IPC query");
    Require(connect(fd.Get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0,
            "Cannot connect isolated WM tree IPC");
    constexpr std::string_view request = "get_tree\n";
    Require(write(fd.Get(), request.data(), request.size()) == static_cast<ssize_t>(request.size()),
            "Cannot request isolated WM tree");

    std::string response;
    std::array<char, 4096> buffer;
    for (;;) {
        const auto count = read(fd.Get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        Require(count >= 0, "Native tree query timed out or failed");
        if (!count) {
            break;
        }
        response.append(buffer.data(), static_cast<std::size_t>(count));
        Require(response.size() <= 1024 * 1024, "Native tree query exceeded its size bound");
    }
    return nlohmann::json::parse(response).get<prism::ipc::TreeMessage>();
}

std::optional<Rect> FindOwner(const prism::ipc::TreeNodeMessage &node, std::string_view app)
{
    if (node.app_id && *node.app_id == app && node.visible.value_or(true) && node.committed_rect) {
        const auto &bounds = *node.committed_rect;
        return Rect{bounds.x, bounds.y, bounds.width, bounds.height};
    }
    if (node.nodes) {
        for (const auto &child : *node.nodes) {
            if (const auto result = FindOwner(child, app)) {
                return result;
            }
        }
    }
    return {};
}

} // namespace prism::tests::owner_task_probe
