#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/theme/compiler.hpp"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <wayland-client.h>

namespace {
using namespace prism;
using namespace std::chrono_literals;

void Require(bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

runtime::ShapedText Shape(std::string_view text, double font)
{
    return {{}, text.size() * font * .5, font};
}

void FindHandle(const runtime::Blueprint &node, std::uint32_t &index, contracts::NodeId &target,
                std::size_t &matches)
{
    const auto own = index++;
    if (node.gesture && node.gesture->action == "group:toggle-immersive") {
        target = {own, 1};
        ++matches;
    }
    for (const auto &child : node.children) {
        FindHandle(child, index, target, matches);
    }
}

class Pointer {
public:
    ~Pointer()
    {
        if (pointer_) {
            zwlr_virtual_pointer_v1_destroy(pointer_);
        }
        if (manager_) {
            zwlr_virtual_pointer_manager_v1_destroy(manager_);
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
        const auto *directory = std::getenv("XDG_RUNTIME_DIR");
        Require(directory && std::filesystem::path(directory).filename().string().starts_with(
                                 "prism-group-session-"),
                "Pointer probe requires its isolated group-session runtime");
        Require(socket && *socket && std::string_view(socket).find('/') == std::string_view::npos,
                "Expected a socket basename within the isolated runtime");
        const auto path = std::filesystem::path(directory) / socket;
        display_ = wl_display_connect(path.c_str());
        Require(display_, "Could not connect to isolated Wayland display");
        registry_ = wl_display_get_registry(display_);
        Require(registry_ && wl_registry_add_listener(registry_, &registry_listener_, this) == 0,
                "Could not attach registry listener");
        Roundtrip();
        Roundtrip();
        Require(manager_ && seat_ && output_ && width_ > 0 && height_ > 0 && scale_ == 1,
                "Expected virtual pointer, seat and one scale-one headless output");
        pointer_ = zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
            manager_, seat_, output_);
        Require(pointer_, "Could not create isolated virtual pointer");
        Roundtrip();
    }

    void Prepare(const char *themes, const char *ui)
    {
        const auto snapshot = theme::LoadTheme(themes, "glass");
        std::ifstream file(ui);
        Require(bool(file), "Could not read packaged Topbar DSL");
        const std::string source{std::istreambuf_iterator<char>{file}, {}};
        auto blueprint = runtime::ParseBlueprint(source);
        contracts::NodeId target;
        std::uint32_t index{};
        std::size_t matches{};
        FindHandle(blueprint, index, target, matches);
        Require(matches == 1, "Expected exactly one group gesture target in packaged Topbar");

        runtime::Scene scene(std::move(blueprint), Shape, contracts::ResourceId{1}, snapshot);
        scene.SetBinding("clock_time", std::string("Oct-01 12:00:00"));
        scene.SetBinding("network_icon", std::string("wifi"));
        scene.SetBinding("power_icon", std::string("power"));
        scene.SetBinding("group_immersive_opacity", 0.0);
        scene.SetViewport({double(width_), snapshot.layout.topbar_surface_height});
        Require(scene.Build(contracts::WindowId{1}).has_value(), "Could not layout Topbar");
        const auto bounds = scene.Bounds(target);
        handle_ = {bounds.x + bounds.width / 2, bounds.y + bounds.height / 2};
        const auto hit = scene.HitTest(handle_);
        Require(hit && hit->node == target && handle_.y + 36 < height_,
                "Packaged Topbar handle is not a valid input target");

        std::cout << "READY " << width_ << ' ' << height_ << ' ' << handle_.x << ' ' << handle_.y
                  << ' ' << snapshot.layout.topbar_surface_height << ' '
                  << snapshot.layout.dock_surface_height << ' ' << snapshot.layout.outer_gap << '\n'
                  << std::flush;
    }

    void Gesture()
    {
        Move(handle_);
        Pause(150ms);
        Button(WL_POINTER_BUTTON_STATE_PRESSED);
        Pause(70ms);
        for (const auto delta : {7.0, 18.0, 36.0}) {
            Move({handle_.x, handle_.y + delta});
            Pause(70ms);
        }
        Button(WL_POINTER_BUTTON_STATE_RELEASED);
        std::cout << "DONE gesture\n" << std::flush;
    }

    void EdgeClick()
    {
        // WM recovery policy reserves the central top six logical pixels.
        Move({width_ / 2.0, 2});
        Pause(100ms);
        Button(WL_POINTER_BUTTON_STATE_PRESSED);
        Pause(70ms);
        Button(WL_POINTER_BUTTON_STATE_RELEASED);
        std::cout << "DONE edge-click\n" << std::flush;
    }

private:
    void Roundtrip()
    {
        Require(wl_display_roundtrip(display_) >= 0, "Wayland connection closed during input");
    }

    void Pause(std::chrono::milliseconds duration)
    {
        std::this_thread::sleep_for(duration);
        Roundtrip();
    }

    static std::uint32_t TimeMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    void Move(contracts::LogicalPoint point)
    {
        zwlr_virtual_pointer_v1_motion_absolute(pointer_, TimeMs(), std::lround(point.x),
                                                std::lround(point.y), width_, height_);
        zwlr_virtual_pointer_v1_frame(pointer_);
        Roundtrip();
    }

    void Button(std::uint32_t state)
    {
        zwlr_virtual_pointer_v1_button(pointer_, TimeMs(), 272, state);
        zwlr_virtual_pointer_v1_frame(pointer_);
        Roundtrip();
    }

    static void Global(void *data, wl_registry *registry, std::uint32_t name, const char *interface,
                       std::uint32_t version)
    {
        auto &self = *static_cast<Pointer *>(data);
        if (std::strcmp(interface, wl_seat_interface.name) == 0 && !self.seat_) {
            self.seat_ =
                static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
        } else if (std::strcmp(interface, wl_output_interface.name) == 0 && !self.output_) {
            self.output_ = static_cast<wl_output *>(
                wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 2u)));
            wl_output_add_listener(self.output_, &output_listener_, &self);
        } else if (std::strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0 &&
                   version >= 2) {
            self.manager_ = static_cast<zwlr_virtual_pointer_manager_v1 *>(
                wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 2));
        }
    }

    static void Removed(void *, wl_registry *, std::uint32_t)
    {
    }

    static void Geometry(void *, wl_output *, std::int32_t, std::int32_t, std::int32_t,
                         std::int32_t, std::int32_t, const char *, const char *, std::int32_t)
    {
    }

    static void Done(void *, wl_output *)
    {
    }

    static void Name(void *, wl_output *, const char *)
    {
    }

    static void Mode(void *data, wl_output *, std::uint32_t flags, std::int32_t width,
                     std::int32_t height, std::int32_t)
    {
        if (flags & WL_OUTPUT_MODE_CURRENT) {
            auto &self = *static_cast<Pointer *>(data);
            self.width_ = width;
            self.height_ = height;
        }
    }

    static void Scale(void *data, wl_output *, std::int32_t scale)
    {
        static_cast<Pointer *>(data)->scale_ = scale;
    }

    static constexpr wl_registry_listener registry_listener_{Global, Removed};
    static constexpr wl_output_listener output_listener_{Geometry, Mode, Done, Scale, Name, Name};
    wl_display *display_{};
    wl_registry *registry_{};
    wl_seat *seat_{};
    wl_output *output_{};
    zwlr_virtual_pointer_manager_v1 *manager_{};
    zwlr_virtual_pointer_v1 *pointer_{};
    std::int32_t width_{}, height_{}, scale_{1};
    contracts::LogicalPoint handle_;
};
} // namespace

int main(int argc, char **argv)
{
    if (argc != 4) {
        std::cerr << "Usage: group_pointer_probe SOCKET THEMES_ROOT TOPBAR_DSL\n";
        return 2;
    }
    try {
        Pointer pointer;
        pointer.Connect(argv[1]);
        pointer.Prepare(argv[2], argv[3]);
        std::string command;
        while (std::getline(std::cin, command) && command != "quit") {
            if (command == "gesture") {
                pointer.Gesture();
            } else if (command == "edge-click") {
                pointer.EdgeClick();
            } else {
                throw std::runtime_error("Unknown bounded pointer probe command");
            }
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
