#include "prism/sdk/client_application.hpp"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <wayland-client.h>

namespace {
using Application = prism::sdk::ClientApplication;
using Stats = prism::sdk::ClientRenderStats;
using namespace std::chrono_literals;

void Require(bool condition, std::string_view detail)
{
    if (!condition) {
        throw std::runtime_error(std::string(detail));
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

        pointer_ = zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
            manager_, seat_, output_);
        Require(pointer_, "Virtual pointer creation failed");
        Flush();
    }

    void Move(std::uint32_t x, std::uint32_t y)
    {
        zwlr_virtual_pointer_v1_motion_absolute(pointer_, ++time_, x, y, 1000, 1000);
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

private:
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
            self.output_ =
                static_cast<wl_output *>(wl_registry_bind(registry, name, &wl_output_interface, 1));
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
    wl_display *display_{};
    wl_registry *registry_{};
    wl_seat *seat_{};
    wl_output *output_{};
    zwlr_virtual_pointer_manager_v1 *manager_{};
    zwlr_virtual_pointer_v1 *pointer_{};
    std::uint32_t time_{};
};

struct ActionObserver {
    unsigned *count;

    void operator()(std::string_view action) const
    {
        Require(action == "activate", "Unexpected action from animated input target");
        ++*count;
    }
};

void PumpFor(Application &app, std::chrono::milliseconds duration)
{
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        Require(app.Pump(20), "Input animation client stopped");
    }
}

void WaitForMapped(Application &app)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!app.IsMapped() || app.FrameCallbackPending() || app.PresentationCount() == 0) {
        Require(std::chrono::steady_clock::now() < deadline, "Input animation client did not map");
        Require(app.Pump(20), "Input animation client stopped during mapping");
    }
}

void Quiet(Application &app, std::string_view scenario)
{
    PumpFor(app, 150ms);
    const auto before = app.GetRenderStats();
    PumpFor(app, 250ms);
    const auto after = app.GetRenderStats();
    Require(before.scene_builds == after.scene_builds &&
                before.gpu_render_attempts == after.gpu_render_attempts &&
                before.swap_attempts == after.swap_attempts && !app.FrameCallbackPending(),
            "Completed input animation kept building/rendering/submitting frames");
    std::cout << "scenario=" << scenario << " swaps=" << after.swap_successes
              << " layouts=" << after.scene_layouts << " builds=" << after.scene_builds << '\n';
}

void Animated(Application &app, const Stats &before, std::string_view scenario,
              std::uint64_t layout_delta = 0)
{
    PumpFor(app, 1000ms);
    const auto after = app.GetRenderStats();
    std::cout << "scenario=" << scenario << " before_swaps=" << before.swap_successes
              << " after_swaps=" << after.swap_successes
              << " before_layouts=" << before.scene_layouts
              << " after_layouts=" << after.scene_layouts << " builds=" << after.scene_builds
              << " frame_pending=" << app.FrameCallbackPending() << '\n'
              << std::flush;
    Require(after.swap_successes >= before.swap_successes + 3,
            "Input state did not request multiple paced animation frames");
    Require(after.scene_layouts == before.scene_layouts + layout_delta,
            "Input animation performed unexpected layout work");
    Require(after.surface_submission_failures == 0, "Input animation submission failed");
    Quiet(app, scenario);
}

void CheckQuantizedMotion(Application &app, RemotePointer &pointer)
{
    pointer.Move(0, 0);
    Require(app.ReplaceUi(R"(
        Card(background:#192A3BFF,padding:24) {
            InteractionTarget(action:"activate") {
                Visual(width:200,height:120,anchor:"center") {
                    Icon("play",width:120,height:120,foreground:#202020FF)
                        .state(when:"hovered",scope:"target",foreground:#212020FF)
                        .transition(property:"foreground",durationMs:1200,easing:"linear")
                }
            }
        }
    )"),
            "Quantized input animation UI failed to install");
    PumpFor(app, 250ms);
    Quiet(app, "input-quantized-initial-idle");

    pointer.Move(500, 500);
    // The initial hover state may submit its unchanged geometry once. Wait for
    // that frame before measuring the eventual one-channel, one-step color change.
    PumpFor(app, 150ms);
    const auto before = app.GetRenderStats();
    const auto end_motion = std::chrono::steady_clock::now() + 250ms;
    while (std::chrono::steady_clock::now() < end_motion) {
        pointer.Move(500, 500);
        Require(app.Pump(1), "Quantized animation stopped during same-target motion");
    }
    // During this color's quantization plateau the worker answers no-pixel
    // opportunities. Same-target motion must preserve its timed fallback sample.
    PumpFor(app, 1300ms);
    const auto after = app.GetRenderStats();
    Require(after.swap_successes > before.swap_successes,
            "Same-target motion lost the quantized animation's timed final sample");
    Require(after.scene_layouts == before.scene_layouts && after.surface_submission_failures == 0,
            "Quantized input animation relaid out or failed submission");
    Quiet(app, "input-quantized-motion-finished");
}

int Verify(const char *socket)
{
    RemotePointer pointer;
    pointer.Connect(socket);
    pointer.Move(0, 0);

    prism::sdk::ClientConfig config;
    config.socket = socket;
    config.app_id = "prism.input.animation.probe";
    config.title = "Input animation verification";
    config.font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    Application app(config);
    unsigned actions = 0;
    app.OnAction(ActionObserver{&actions});
    Require(app.Open(R"(
        Card(background:#192A3BFF,padding:24) {
            InteractionTarget(width:$targetWidth,action:"activate") {
                Visual(width:200,height:12,anchor:"center",background:#EE7799FF)
                    .state(when:"hovered",scope:"target",scaleX:1.4)
                    .state(when:"pressed",scope:"target",scaleX:0.7)
                    .transition(property:"scaleX",durationMs:600,easing:"linear")
            }
        }
    )"),
            "Input animation UI failed to open");
    Require(app.SetBinding("targetWidth", 1.0), "Target width binding unavailable");
    WaitForMapped(app);
    Require(app.GlRenderer().find("V3D") != std::string::npos,
            "Input animation verification requires V3D");
    Quiet(app, "input-initial-idle");

    // A stationary pointer enters a target solely because layout moves its edge.
    // This establishes a track from Build(), after the input event has finished.
    pointer.Move(500, 500);
    Quiet(app, "input-outside-target-idle");
    auto before = app.GetRenderStats();
    Require(app.SetBinding("targetWidth", 0.0), "Expanding target failed");
    Animated(app, before, "input-layout-rehit-finished", 1);

    before = app.GetRenderStats();
    pointer.Button(true);
    Animated(app, before, "input-press-finished");
    Require(actions == 0, "Press dispatched the action before release");
    before = app.GetRenderStats();
    pointer.Button(false);
    Animated(app, before, "input-release-finished");
    Require(actions == 1, "Release did not dispatch exactly one activation");

    before = app.GetRenderStats();
    pointer.Move(0, 0);
    Animated(app, before, "input-leave-finished");
    before = app.GetRenderStats();
    pointer.Move(500, 500);
    Animated(app, before, "input-hover-finished");

    CheckQuantizedMotion(app, pointer);

    // A replacement UI must retire the active input timeline and its worker
    // opportunities. Physical devices share a logical Wayland pointer source;
    // source removal itself is covered by the protocol capability fixture.
    pointer.Move(0, 0);
    PumpFor(app, 40ms);
    Require(app.ReplaceUi("Card(background:#192A3BFF) { Icon(\"check\") }"),
            "Replacing an animated input UI failed");
    PumpFor(app, 250ms);
    Quiet(app, "input-replacement-cancel-finished");
    app.Close();
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: sdk_input_animation_probe <socket>\n";
        return 2;
    }
    try {
        return Verify(argv[1]);
    } catch (const std::exception &error) {
        std::cerr << "SDK input animation probe failed: " << error.what() << '\n';
        return 1;
    }
}
