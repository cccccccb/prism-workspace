#include "prism/sdk/client_application.hpp"
#include "prism/theme/compiler.hpp"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <wayland-client.h>

namespace {
using Application = prism::sdk::ClientApplication;
using namespace std::chrono_literals;

void Require(bool value, std::string_view message)
{
    if (!value) {
        throw std::runtime_error(std::string(message));
    }
}

class RemotePointer {
public:
    ~RemotePointer()
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
        display_ = wl_display_connect(socket);
        Require(display_, "Input probe could not connect");
        registry_ = wl_display_get_registry(display_);
        Require(registry_ && wl_registry_add_listener(registry_, &listener_, this) == 0,
                "Input probe registry unavailable");
        Require(wl_display_roundtrip(display_) >= 0, "Input probe registry dispatch failed");
        Require(seat_ && manager_ && output_, "Virtual pointer protocol unavailable");
        Require(wl_display_roundtrip(display_) >= 0, "Output geometry dispatch failed");
        Require(width_ > 0 && height_ > 0 && scale_ == 1 &&
                    transform_ == WL_OUTPUT_TRANSFORM_NORMAL,
                "Snapshot probe requires an untransformed scale-one isolated output");

        pointer_ = zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
            manager_, seat_, output_);
        Require(pointer_, "Virtual pointer creation failed");
        Flush();
    }

    void Move(std::uint32_t x, std::uint32_t y)
    {
        zwlr_virtual_pointer_v1_motion_absolute(pointer_, ++time_, x, y, width_, height_);
        zwlr_virtual_pointer_v1_frame(pointer_);
        Flush();
    }

    void Button(bool down)
    {
        zwlr_virtual_pointer_v1_button(pointer_, ++time_, 272,
                                       down ? WL_POINTER_BUTTON_STATE_PRESSED
                                            : WL_POINTER_BUTTON_STATE_RELEASED);
        zwlr_virtual_pointer_v1_frame(pointer_);
        Flush();
    }

    std::uint32_t Width() const
    {
        return width_;
    }

    std::uint32_t Height() const
    {
        return height_;
    }

    void Click(std::uint32_t x, std::uint32_t y)
    {
        Move(x, y);
        Button(true);
        Button(false);
    }

private:
    static void Geometry(void *data, wl_output *, std::int32_t, std::int32_t, std::int32_t,
                         std::int32_t, std::int32_t, const char *, const char *,
                         std::int32_t transform)
    {
        static_cast<RemotePointer *>(data)->transform_ = transform;
    }

    static void Mode(void *data, wl_output *, std::uint32_t flags, std::int32_t width,
                     std::int32_t height, std::int32_t)
    {
        if ((flags & WL_OUTPUT_MODE_CURRENT) == 0) {
            return;
        }
        auto &self = *static_cast<RemotePointer *>(data);
        self.width_ = width;
        self.height_ = height;
    }

    static void Done(void *, wl_output *)
    {
    }

    static void Scale(void *data, wl_output *, std::int32_t scale)
    {
        static_cast<RemotePointer *>(data)->scale_ = scale;
    }

    void Flush()
    {
        Require(wl_display_roundtrip(display_) >= 0, "Virtual pointer dispatch failed");
    }

    static void Global(void *data, wl_registry *registry, std::uint32_t name, const char *interface,
                       std::uint32_t version)
    {
        auto &self = *static_cast<RemotePointer *>(data);
        if (std::strcmp(interface, wl_seat_interface.name) == 0 && !self.seat_) {
            self.seat_ = static_cast<wl_seat *>(
                wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
        } else if (std::strcmp(interface, wl_output_interface.name) == 0 && !self.output_) {
            self.output_ = static_cast<wl_output *>(
                wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 2u)));
            Require(wl_output_add_listener(self.output_, &output_listener_, &self) == 0,
                    "Output geometry listener unavailable");
        } else if (std::strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0 &&
                   version >= 2) {
            self.manager_ = static_cast<zwlr_virtual_pointer_manager_v1 *>(
                wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 2));
        }
    }

    static void Remove(void *, wl_registry *, std::uint32_t)
    {
    }

    inline static const wl_registry_listener listener_{Global, Remove};
    inline static const wl_output_listener output_listener_{Geometry, Mode, Done, Scale};
    wl_display *display_{};
    wl_registry *registry_{};
    wl_seat *seat_{};
    wl_output *output_{};
    zwlr_virtual_pointer_manager_v1 *manager_{};
    zwlr_virtual_pointer_v1 *pointer_{};
    std::uint32_t time_{};
    std::uint32_t width_{};
    std::uint32_t height_{};
    std::int32_t scale_{1};
    std::int32_t transform_{};
};

constexpr std::string_view popup_ui = R"(
    Card(background:#193A5FFF,inputShape:"bounds") {
        Visual(background:$rootTint)
            .transition(property:"background",durationMs:240,easing:"linear")
        VStack {
            Card(height:$lead)
            Button("Open",action:"open",width:160,height:120,background:#708090FF)
        }
        Popup("open",width:280,height:200,padding:16,material:"panel",backdropBlur:$blur) {
            Contour(recipe:"attachedPanel",radius:12,neckWidth:28,neckHeight:8,fallback:"detached")
            VStack(spacing:12) {
                Checkbox(action:"child",checked:$checked,height:48,enabled:$enabled) {
                    Visual(background:$tint)
                        .transition(property:"background",durationMs:240,easing:"linear")
                    Text("Apply",height:20,foreground:#102030FF)
                }
                Text("Child-local controls",height:20,foreground:#102030FF)
                Slider(action:"volume",value:$volume,height:32) {
                    Visual(sliderPart:"track",height:4,background:#8090A0FF)
                    Visual(sliderPart:"fill",height:4,background:#3355AAFF)
                    Visual(sliderPart:"thumb",width:12,height:12,background:#102030FF)
                }
                Button("Command",action:"command",height:32)
            }
        }
    }
)";

class Probe {
public:
    void Action(std::string_view action)
    {
        if (action == "command") {
            ++commands_;
        }
    }

    void Submitted(prism::runtime::UiLoadId)
    {
        ++root_milestones_;
    }

    void Control(const prism::runtime::ControlEdit &edit)
    {
        if (edit.action == "volume") {
            ++edits_;
        } else if (edit.action == "child") {
            ++actions_;
        }
    }

    void PumpFor(Application &app, std::chrono::milliseconds duration)
    {
        const auto until = std::chrono::steady_clock::now() + duration;
        do {
            Require(app.Pump(10), "SDK stopped");
        } while (std::chrono::steady_clock::now() < until);
    }

    void Settle(Application &app)
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        auto quiet = std::chrono::steady_clock::now();
        auto status = app.GetPlatformStatus();
        while (std::chrono::steady_clock::now() < deadline) {
            Require(app.Pump(10), "SDK stopped while settling");
            const auto next = app.GetPlatformStatus();
            if (next.surface_pixel_commits != status.surface_pixel_commits ||
                next.popup_pixel_commits != status.popup_pixel_commits ||
                next.popup_lifetime != status.popup_lifetime) {
                quiet = std::chrono::steady_clock::now();
            }
            status = next;
            if (next.surface_pixel_commits && !app.FrameCallbackPending() &&
                std::chrono::steady_clock::now() - quiet >= 100ms) {
                return;
            }
        }
        throw std::runtime_error("SDK did not become quiescent");
    }

    void WaitPopup(Application &app, bool open)
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            Require(app.Pump(10), "SDK stopped while waiting for popup");
            const auto state = app.GetPlatformStatus();
            if (open ? state.popup_lifetime && state.popup_pixel_commits > previous_pixels_
                     : !state.popup_lifetime) {
                Settle(app);
                return;
            }
        }
        const auto state = app.GetPlatformStatus();
        throw std::runtime_error(
            std::string(open ? "native Popup did not submit" : "native Popup did not close") +
            " lifetime=" + std::to_string(state.popup_lifetime) +
            " configures=" + std::to_string(state.popup_configures) +
            " child_pixels=" + std::to_string(state.popup_pixel_commits) +
            " root_pixels=" + std::to_string(state.surface_pixel_commits) +
            " parent_configure=" + std::to_string(state.configure_count));
    }

    void Run(const char *socket, const std::filesystem::path &root)
    {
        RemotePointer pointer;
        pointer.Connect(socket);
        pointer.Move(0, 0);
        prism::sdk::ClientConfig config;
        config.socket = socket;
        config.app_id = "prism.popup.sdk.probe";
        config.title = "Popup SDK integration";
        config.font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
        Application app(config);
        app.OnAction(std::bind_front(&Probe::Action, this));
        app.OnControlValue(std::bind_front(&Probe::Control, this));
        app.OnUiSubmitted(std::bind_front(&Probe::Submitted, this));
        const auto square =
            prism::theme::LoadTheme(root / "resources/themes", "square", 1, "light");
        Require(app.ApplyTheme(square), "Square theme failed");
        const auto prepared = prism::runtime::PrepareComponent(popup_ui);
        prism::runtime::LoadDiagnostic diagnostic;
        const bool opened = app.OpenPrepared(app.BeginUiLoad(), prepared, &diagnostic);
        Require(opened, "Popup SDK UI failed to open: " + diagnostic.message);
        Require(app.SetBinding("blur", 12.0) && app.SetBinding("lead", 200.0) &&
                    app.SetBinding("enabled", true) && app.SetBinding("checked", false) &&
                    app.SetBinding("volume", 0.25) &&
                    app.SetBinding("rootTint", prism::contracts::Color{25, 42, 59, 255}) &&
                    app.SetBinding("tint", prism::contracts::Color{70, 110, 180, 255}),
                "initial bindings rejected");
        PumpFor(app, 100ms);
        Settle(app);
        Require(app.GlRenderer().find("V3D") != std::string::npos,
                "SDK integration requires V3D, actual=" + app.GlRenderer());
        const auto metrics = app.GetPlatformStatus().metrics;
        Require(metrics.scale == 1 && metrics.logical_size.width == pointer.Width() &&
                    metrics.logical_size.height == pointer.Height(),
                "Integration gate requires a single fullscreen isolated client");
        Require(app.SetBinding("lead", metrics.logical_size.height / 2 - 60),
                "anchor lead rejected");
        Settle(app);
        std::cout << "opening at 80," << pointer.Height() / 2
                  << " root_pixels=" << app.GetPlatformStatus().surface_pixel_commits << std::endl;
        pointer.Click(80, pointer.Height() / 2);
        WaitPopup(app, true);
        const auto initial = app.GetPlatformStatus();
        Require(initial.popup_configures == 1 && initial.popup_pixel_commits >= 1,
                "native configuration or first submission missing");
        const auto body = initial.popup_window_bounds;
        const auto x = std::uint32_t(body.x + 60);
        const auto y = std::uint32_t(body.y + 8 + 16 + 24);
        pointer.Click(x, y);
        Settle(app);
        Require(actions_ == 1, "native child button did not activate");
        Require(app.GetPlatformStatus().popup_lifetime == initial.popup_lifetime,
                "child input recreated its native lifetime");

        // Enabled is metadata-only; no GPU frame is needed to reject its action.
        Require(app.SetBinding("enabled", false), "enabled update rejected");
        Settle(app);
        const auto disabled_pixels = app.GetPlatformStatus().popup_pixel_commits;
        pointer.Click(x, y);
        Settle(app);
        Require(actions_ == 1 && app.GetPlatformStatus().popup_lifetime == initial.popup_lifetime,
                "disabled child consumed action or incorrectly dismissed as outside");
        Require(app.GetPlatformStatus().popup_pixel_commits == disabled_pixels,
                "metadata-only click forced child pixels");
        Require(app.SetBinding("enabled", true), "re-enable rejected");
        Settle(app);
        pointer.Click(x, y);
        Settle(app);
        Require(actions_ == 2, "newly adopted metadata failed to restore action");

        const auto before_animation = app.GetPlatformStatus();
        const auto milestones = root_milestones_;
        Require(app.SetBinding("tint", prism::contracts::Color{220, 80, 120, 255}),
                "child paint animation failed to start");
        PumpFor(app, 400ms);
        Settle(app);
        const auto animated = app.GetPlatformStatus();
        Require(animated.popup_pixel_commits >= before_animation.popup_pixel_commits + 3,
                "child animation did not receive independently paced submissions");
        Require(animated.surface_pixel_commits == before_animation.surface_pixel_commits &&
                    root_milestones_ == milestones,
                "child-only updates redrew root or advanced root UI milestone");
        std::cout << "child_motion root_pixels="
                  << animated.surface_pixel_commits - before_animation.surface_pixel_commits
                  << " child_pixels="
                  << animated.popup_pixel_commits - before_animation.popup_pixel_commits
                  << std::endl;

        Require(app.SetBinding("rootTint", prism::contracts::Color{40, 75, 110, 255}),
                "root animation failed to start");
        PumpFor(app, 400ms);
        Settle(app);
        const auto root_animated = app.GetPlatformStatus();
        Require(root_animated.surface_pixel_commits >= animated.surface_pixel_commits + 3 &&
                    root_animated.popup_pixel_commits == animated.popup_pixel_commits,
                "root motion was not independently paced or redrew stable child");
        std::cout << "root_motion root_pixels="
                  << root_animated.surface_pixel_commits - animated.surface_pixel_commits
                  << " child_pixels="
                  << root_animated.popup_pixel_commits - animated.popup_pixel_commits << std::endl;

        Require(app.SetBinding("rootTint", prism::contracts::Color{60, 100, 140, 255}) &&
                    app.SetBinding("tint", prism::contracts::Color{90, 180, 110, 255}),
                "mixed animation failed to start");
        PumpFor(app, 400ms);
        Settle(app);
        const auto mixed = app.GetPlatformStatus();
        Require(mixed.surface_pixel_commits >= root_animated.surface_pixel_commits + 3 &&
                    mixed.popup_pixel_commits >= root_animated.popup_pixel_commits + 3,
                "one target starved the other target's animation");
        std::cout << "mixed_motion root_pixels="
                  << mixed.surface_pixel_commits - root_animated.surface_pixel_commits
                  << " child_pixels="
                  << mixed.popup_pixel_commits - root_animated.popup_pixel_commits << std::endl;

        pointer.Move(std::uint32_t(body.x + 35),
                     std::uint32_t(body.y + 8 + 16 + 48 + 12 + 20 + 12 + 16));
        pointer.Button(true);
        pointer.Move(std::uint32_t(body.x + 210),
                     std::uint32_t(body.y + 8 + 16 + 48 + 12 + 20 + 12 + 16));
        pointer.Button(false);
        Settle(app);
        Require(edits_ > 0, "native local Slider did not produce a control edit");

        const auto before_effects = app.GetPlatformStatus();
        Require(app.SetBinding("blur", 24.0), "backdrop metadata binding rejected");
        Settle(app);
        const auto effect_update = app.GetPlatformStatus();
        Require(effect_update.popup_lifetime == initial.popup_lifetime &&
                    effect_update.popup_pixel_commits == before_effects.popup_pixel_commits &&
                    effect_update.surface_pixel_commits == before_effects.surface_pixel_commits,
                "blur-only change redrew a target or recreated its lifetime");
        Require(app.SetBinding("blur", 0.0), "backdrop clear binding rejected");
        Settle(app);
        Require(app.GetPlatformStatus().popup_pixel_commits == effect_update.popup_pixel_commits,
                "clearing backdrop redrew child pixels");
        Require(app.SetBinding("blur", 12.0), "backdrop restore binding rejected");
        Settle(app);
        std::cout << "backdrop_metadata root_pixels=0 child_pixels=0 lifetime_stable=1"
                  << std::endl;

        const auto glass = prism::theme::LoadTheme(root / "resources/themes", "glass", 2, "dark");
        Require(app.ApplyTheme(glass), "Glass theme failed");
        Settle(app);
        Require(app.GetPlatformStatus().popup_lifetime == initial.popup_lifetime,
                "Glass unnecessarily fell back or recreated native Popup");
        std::uint64_t theme_generation = 3;
        for (const auto material : {"glass", "translucent", "transparent", "square"}) {
            for (const auto mode : {"light", "dark"}) {
                const auto candidate = prism::theme::LoadTheme(root / "resources/themes", material,
                                                               theme_generation++, mode);
                Require(app.ApplyTheme(candidate), "material/mode theme failed");
                Settle(app);
                Require(app.GetPlatformStatus().popup_lifetime == initial.popup_lifetime,
                        "material/mode switch lost native Popup");
            }
        }
        auto again = square;
        again.generation = theme_generation;
        Require(app.ApplyTheme(again), "Square theme restore failed");
        Settle(app);
        std::cout << "native_materials combinations=8 lifetime_stable=1" << std::endl;

        const auto reopened = app.GetPlatformStatus();
        const auto command_body = reopened.popup_window_bounds;
        pointer.Click(std::uint32_t(command_body.x + 60),
                      std::uint32_t(command_body.y + 8 + 16 + 48 + 12 + 20 + 12 + 32 + 12 + 16));
        WaitPopup(app, false);
        Require(commands_ == 1, "native command did not dispatch and dismiss logical Popup");
        previous_pixels_ = app.GetPlatformStatus().popup_pixel_commits;
        pointer.Click(80, pointer.Height() / 2);
        WaitPopup(app, true);
        pointer.Click(pointer.Width() - 40, pointer.Height() / 2);
        WaitPopup(app, false);
        previous_pixels_ = app.GetPlatformStatus().popup_pixel_commits;
        pointer.Click(80, pointer.Height() / 2);
        WaitPopup(app, true);
        Require(app.ReplaceUi("Card(background:#334455FF){Text(\"Replacement\")}"),
                "UI replacement failed with open child");
        WaitPopup(app, false);
        Require(app.IsMapped() && app.GetPlatformStatus().popup_closes >= reopened.popup_closes + 2,
                "child retirement damaged persistent parent");
        app.Close();
        app.Close();
        std::cout << "sdk_popup_surface_probe: passed V3D native_input metadata animation "
                     "capability_fallback"
                  << " root_milestones=" << root_milestones_ << " actions=" << actions_
                  << " edits=" << edits_ << " commands=" << commands_ << '\n';
    }

private:
    unsigned actions_{}, edits_{}, commands_{}, root_milestones_{};
    std::uint64_t previous_pixels_{};
};
} // namespace

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::cerr << "usage: sdk_popup_surface_probe <socket> <repository>\n";
        return 2;
    }
    try {
        Probe probe;
        probe.Run(argv[1], argv[2]);
    } catch (const std::exception &error) {
        std::cerr << "sdk_popup_surface_probe: " << error.what() << '\n';
        return 1;
    }
}
