#include "prism/sdk/client_application.hpp"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <wayland-client.h>

namespace {
using Application = prism::sdk::ClientApplication;
using namespace std::chrono_literals;

struct ActionObserver;
struct GestureObserver;
Application *failure_application{};
ActionObserver *failure_actions{};
GestureObserver *failure_gestures{};
std::thread::id failure_owner{};

void PrintFailure(std::string_view detail);

void Require(bool condition, std::string_view detail)
{
    if (!condition) {
        PrintFailure(detail);
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

constexpr std::string_view moving_ui = R"(
    Card(background:#192A3BFF,paddingX:$targetX) {
        InteractionTarget(width:$targetWidth,action:"activate") {
            Visual(background:#EE7799FF)
        }
    }
)";

constexpr std::string_view replacement_ui = R"(
    Card(background:#192A3BFF,paddingX:240) {
        InteractionTarget(width:40,action:"replacement") {
            Visual(background:#55AAFFFF)
        }
    }
)";

struct ActionObserver {
    Application *app{};
    std::vector<std::string> actions;
    bool replace_on_action{};

    void Handle(std::string_view action)
    {
        actions.emplace_back(action);
        if (replace_on_action) {
            replace_on_action = false;
            Require(app->ReplaceUi(replacement_ui), "Action replacement failed");
        }
    }
};

class ScopedFailureContext {
public:
    ScopedFailureContext(Application &app, ActionObserver &actions)
    {
        failure_application = &app;
        failure_actions = &actions;
        failure_owner = std::this_thread::get_id();
    }

    ~ScopedFailureContext()
    {
        failure_application = nullptr;
        failure_actions = nullptr;
    }

    ScopedFailureContext(const ScopedFailureContext &) = delete;
    ScopedFailureContext &operator=(const ScopedFailureContext &) = delete;
};

void WaitForActions(Application &app, const ActionObserver &actions, std::size_t expected,
                    std::string_view action)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (actions.actions.size() < expected) {
        Require(std::chrono::steady_clock::now() < deadline,
                "Expected action never reached the UI owner");
        Require(app.Pump(20), "Input client stopped while waiting for action receipt");
    }
    Require(actions.actions.size() == expected && actions.actions.back() == action,
            "Action receipt had an unexpected count or action");
}

void PumpFor(Application &app, std::chrono::milliseconds duration)
{
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        Require(app.Pump(20), "Input snapshot client stopped");
    }
}

void WaitForMapped(Application &app)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!app.IsMapped() || app.FrameCallbackPending() || app.PresentationCount() == 0) {
        Require(std::chrono::steady_clock::now() < deadline, "Input snapshot client did not map");
        Require(app.Pump(20), "Input snapshot client stopped during mapping");
    }
}

void WaitForNewPixels(Application &app, std::uint64_t previous)
{
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + 5s;
    while (app.GetRenderStats().swap_successes <= previous) {
        Require(std::chrono::steady_clock::now() < deadline,
                "Moved target never reached a successful pixel submission");
        Require(app.Pump(20), "Input snapshot client stopped waiting for pixels");
    }
    std::cout << "pixel-confirmation-us="
              << std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - started)
                     .count()
              << '\n';
}

void Stable(Application &app, std::string_view scenario)
{
    PumpFor(app, 180ms);
    const auto before = app.GetRenderStats();
    PumpFor(app, 180ms);
    const auto after = app.GetRenderStats();
    Require(before.scene_builds == after.scene_builds &&
                before.swap_attempts == after.swap_attempts && !app.FrameCallbackPending(),
            "Snapshot probe did not reach a stable submitted frame");
    Require(after.surface_submission_failures == 0, "Snapshot submission failed");
    std::cout << "scenario=" << scenario << " swaps=" << after.swap_successes
              << " layouts=" << after.scene_layouts << " builds=" << after.scene_builds << '\n'
              << std::flush;
}

void HoldUiForWorker()
{
    // Each injected request has completed a compositor roundtrip. Leave the UI
    // owner idle while the independent SDK worker receives the resulting input.
    // This is a scheduling allowance; no public SDK input-receipt fence exists.
    std::this_thread::sleep_for(120ms);
}

constexpr std::string_view gesture_ui = R"(
    Card(background:#192A3BFF,paddingX:240) {
        InteractionTarget(width:80,action:"gesture-click",visible:$targetVisible) {
            Visual(background:#EE7799FF)
                .state(when:"dragging",scope:"target",opacity:0.75)
        }.gesture(action:"generic-drag",threshold:6)
    }
)";

struct GestureObserver {
    Application *app{};
    std::thread::id owner{std::this_thread::get_id()};
    std::vector<prism::contracts::GestureEvent> events;
    bool replace_on_begin{};
    bool hide_on_update{};

    void Handle(const prism::contracts::GestureEvent &event)
    {
        Require(std::this_thread::get_id() == owner, "Gesture callback left the UI owner thread");
        events.push_back(event);
        if (replace_on_begin && event.phase == prism::contracts::GesturePhase::Begin) {
            replace_on_begin = false;
            Require(app->ReplaceUi(replacement_ui), "Gesture callback UI replacement failed");
        } else if (hide_on_update && event.phase == prism::contracts::GesturePhase::Update) {
            hide_on_update = false;
            Require(app->SetBinding("targetVisible", false), "Gesture callback hide failed");
        }
    }
};

const char *PhaseName(prism::contracts::GesturePhase phase)
{
    using Phase = prism::contracts::GesturePhase;
    switch (phase) {
    case Phase::Begin:
        return "Begin";
    case Phase::Update:
        return "Update";
    case Phase::End:
        return "End";
    case Phase::Cancel:
        return "Cancel";
    }
    return "Unknown";
}

void PrintFailure(std::string_view detail)
{
    std::cerr << "receipt-failure=" << detail << '\n';
    if (failure_owner != std::this_thread::get_id()) {
        std::cerr << "diagnostics-unavailable=outside-ui-owner\n";
        return;
    }
    if (failure_actions) {
        std::cerr << "action-count=" << failure_actions->actions.size();
        for (const auto &action : failure_actions->actions) {
            std::cerr << " action=" << action;
        }
        std::cerr << '\n';
    }
    if (failure_gestures) {
        std::cerr << "gesture-count=" << failure_gestures->events.size() << '\n';
        for (const auto &event : failure_gestures->events) {
            std::cerr << "gesture phase=" << PhaseName(event.phase) << " id=" << event.id
                      << " serial=" << event.serial << " start=" << event.start.x << ','
                      << event.start.y << " position=" << event.position.x << ','
                      << event.position.y << " snapshot=" << event.snapshot_scene << ':'
                      << event.snapshot_version << " time-ns=" << event.time_ns << '\n';
        }
    }
    if (failure_application) {
        const auto stats = failure_application->GetRenderStats();
        const auto platform = failure_application->GetPlatformStatus();
        std::cerr << "client builds=" << stats.scene_builds << " layouts=" << stats.scene_layouts
                  << " swap-attempts=" << stats.swap_attempts << " swaps=" << stats.swap_successes
                  << " submission-failures=" << stats.surface_submission_failures
                  << " configure=" << platform.configure_count
                  << " frame-done=" << platform.frame_done_count
                  << " presentation=" << platform.presentation_count
                  << " frame-pending=" << failure_application->FrameCallbackPending() << '\n';
    }
}

void WaitForGestures(Application &app, const GestureObserver &gestures,
                     std::initializer_list<prism::contracts::GesturePhase> phases)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (true) {
        Require(gestures.events.size() <= phases.size(),
                "Unexpected extra gesture while waiting for input receipt");
        auto expected_phase = phases.begin();
        for (const auto &event : gestures.events) {
            Require(event.phase == *expected_phase++,
                    "Unexpected gesture phase while waiting for input receipt");
            const auto &first = gestures.events.front();
            Require(event.id == first.id && event.serial == first.serial &&
                        event.start == first.start,
                    "Gesture capture identity changed while waiting for input receipt");
        }
        if (gestures.events.size() == phases.size()) {
            return;
        }

        Require(std::chrono::steady_clock::now() < deadline,
                "Expected gesture never reached the UI owner");
        Require(app.Pump(20), "Input client stopped while waiting for gesture receipt");
    }
}

class ScopedGestureHandler {
public:
    ScopedGestureHandler(Application &app, GestureObserver &observer) : app_(app)
    {
        failure_gestures = &observer;
        app_.OnGesture(std::bind_front(&GestureObserver::Handle, &observer));
    }

    ~ScopedGestureHandler()
    {
        // A failed assertion must unbind before the local observer is destroyed.
        // Otherwise application teardown can deliver Cancel into its freed events.
        app_.OnGesture({});
        failure_gestures = nullptr;
    }

    ScopedGestureHandler(const ScopedGestureHandler &) = delete;
    ScopedGestureHandler &operator=(const ScopedGestureHandler &) = delete;

private:
    Application &app_;
};

void CheckGestures(Application &app, RemotePointer &pointer, ActionObserver &actions,
                   std::uint32_t x, std::uint32_t y)
{
    using Phase = prism::contracts::GesturePhase;
    GestureObserver gestures{&app};
    ScopedGestureHandler gesture_handler(app, gestures);
    Require(app.ReplaceUi(gesture_ui), "Gesture UI replacement failed");
    Require(app.SetBinding("targetVisible", true), "Gesture target show failed");
    Stable(app, "gesture-submission");
    const auto old_actions = actions.actions.size();
    pointer.Move(x, y);
    pointer.Button(true);
    pointer.Move(x + 3, y);
    Stable(app, "gesture-below-threshold");
    Require(gestures.events.empty(), "Gesture started before its movement threshold");
    pointer.Button(false);
    WaitForActions(app, actions, old_actions + 1, "gesture-click");
    Stable(app, "gesture-ordinary-click");
    Require(actions.actions.size() == old_actions + 1 && actions.actions.back() == "gesture-click",
            "Gesture declaration broke ordinary click semantics");

    pointer.Move(x, y);
    pointer.Button(true);
    pointer.Move(x + 12, y);
    WaitForGestures(app, gestures, {Phase::Begin});
    Stable(app, "gesture-begin");
    Require(gestures.events.size() == 1 && gestures.events[0].phase == Phase::Begin &&
                gestures.events[0].serial && gestures.events[0].snapshot_scene &&
                gestures.events[0].snapshot_version && !gestures.events[0].touch,
            "Gesture Begin lacks the original Down or submitted snapshot provenance");
    const auto first = gestures.events[0];
    pointer.Move(x + 24, y);
    WaitForGestures(app, gestures, {Phase::Begin, Phase::Update});
    Stable(app, "gesture-update");
    Require(gestures.events.size() == 2 && gestures.events[1].phase == Phase::Update &&
                gestures.events[1].id == first.id && gestures.events[1].serial == first.serial &&
                gestures.events[1].start == first.start,
            "Gesture Update changed capture identity or failed");
    pointer.Button(false);
    WaitForGestures(app, gestures, {Phase::Begin, Phase::Update, Phase::End});
    Stable(app, "gesture-end");
    Require(gestures.events.size() == 3 && gestures.events.back().phase == Phase::End &&
                gestures.events.back().id == first.id && actions.actions.size() == old_actions + 1,
            "Completed drag fired a click or lost End");

    gestures.events.clear();
    gestures.replace_on_begin = true;
    pointer.Move(x, y);
    pointer.Button(true);
    pointer.Move(x + 12, y);
    WaitForGestures(app, gestures, {Phase::Begin, Phase::Cancel});
    Stable(app, "gesture-callback-replaces-ui");
    Require(gestures.events.size() == 2 && gestures.events[0].phase == Phase::Begin &&
                gestures.events[1].phase == Phase::Cancel &&
                gestures.events[0].id == gestures.events[1].id && !gestures.replace_on_begin,
            "UI replacement from Begin failed to cancel exactly once");
    const auto retired = gestures.events[0].id;
    pointer.Move(x + 24, y);
    pointer.Button(false);
    Stable(app, "gesture-retired-ui-tail");
    Require(gestures.events.size() == 2 && actions.actions.size() == old_actions + 1,
            "Old gesture tail revived an action in replacement UI");

    Require(app.ReplaceUi(gesture_ui), "Second gesture UI replacement failed");
    Require(app.SetBinding("targetVisible", true), "Second gesture target show failed");
    Stable(app, "gesture-second-submission");
    gestures.events.clear();
    gestures.hide_on_update = true;
    pointer.Move(x, y);
    pointer.Button(true);
    pointer.Move(x + 12, y);
    WaitForGestures(app, gestures, {Phase::Begin});
    Stable(app, "gesture-second-begin");
    pointer.Move(x + 24, y);
    WaitForGestures(app, gestures, {Phase::Begin, Phase::Update, Phase::Cancel});
    Stable(app, "gesture-callback-hides-target");
    Require(gestures.events.size() == 3 && gestures.events[0].phase == Phase::Begin &&
                gestures.events[0].id > retired && gestures.events[1].phase == Phase::Update &&
                gestures.events[2].phase == Phase::Cancel && !gestures.hide_on_update,
            "Binding mutation from Update failed to cancel the stream");
    pointer.Button(false);
    Stable(app, "gesture-hidden-release");
    Require(gestures.events.size() == 3 && actions.actions.size() == old_actions + 1,
            "Hidden gesture released as a click");

    Require(app.SetBinding("targetVisible", true), "Final gesture target show failed");
    Stable(app, "gesture-close-submission");
    gestures.events.clear();
    pointer.Move(x, y);
    pointer.Button(true);
    pointer.Move(x + 12, y);
    WaitForGestures(app, gestures, {Phase::Begin});
    Stable(app, "gesture-before-close");
    app.Close();
    Require(gestures.events.size() == 2 && gestures.events[0].phase == Phase::Begin &&
                gestures.events[1].phase == Phase::Cancel,
            "Close did not deliver gesture cancellation");
    app.Close();
    Require(gestures.events.size() == 2, "Repeated Close duplicated gesture cancellation");
    app.OnGesture({});
    pointer.Button(false);
}

int Verify(const char *socket)
{
    RemotePointer pointer;
    pointer.Connect(socket);
    pointer.Move(0, 0);

    prism::sdk::ClientConfig config;
    config.socket = socket;
    config.app_id = "prism.input.snapshot.probe";
    config.title = "Input snapshot verification";
    config.font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    ActionObserver observer;
    Application app(config);
    observer.app = &app;
    ScopedFailureContext failure_context(app, observer);
    app.OnAction(std::bind_front(&ActionObserver::Handle, &observer));
    Require(app.Open(moving_ui), "Input snapshot UI failed to open");
    Require(app.SetBinding("targetX", 40.0), "Initial target position rejected");
    Require(app.SetBinding("targetWidth", 120.0), "Initial target width rejected");
    WaitForMapped(app);
    Require(app.GlRenderer().find("V3D") != std::string::npos,
            "Input snapshot verification requires V3D");
    Stable(app, "snapshot-initial-submission");

    // Run as the only normal window on the isolated WM. Its horizontal outer
    // gaps are symmetric; the target fills the client height, so output center
    // stays away from the topbar and dock reservations. Baseline input below
    // verifies that this mapping actually reaches the intended target.
    const auto client_width = app.GetPlatformStatus().metrics.logical_size.width;
    Require(client_width >= 520 && client_width <= pointer.Width(),
            "Snapshot probe requires a single sufficiently wide client window");
    const auto origin_x =
        static_cast<std::uint32_t>(std::lround((pointer.Width() - client_width) / 2));
    const auto old_x = origin_x + 80;
    const auto new_x = origin_x + 260;
    const auto y = pointer.Height() / 2;
    pointer.Click(old_x, y);
    WaitForActions(app, observer, 1, "activate");
    Stable(app, "snapshot-baseline-click");
    Require(observer.actions == std::vector<std::string>{"activate"},
            "Baseline pointer mapping missed the submitted target");

    const auto before_move = app.GetRenderStats();
    pointer.Click(old_x, y);
    HoldUiForWorker();
    Require(app.SetBinding("targetX", 240.0), "Target move rejected");
    Require(app.SetBinding("targetWidth", 40.0), "Target shrink rejected");
    Require(observer.actions.size() == 1, "Action ran while the UI owner was paused");
    // Quiescent counters alone cannot distinguish idle from an in-flight GPU
    // submission. First require positive progress; then check settled state.
    WaitForNewPixels(app, before_move.swap_successes);
    WaitForActions(app, observer, 2, "activate");
    Stable(app, "snapshot-queued-old-geometry");
    Require(observer.actions == std::vector<std::string>({"activate", "activate"}),
            "Queued input used the unsubmitted candidate instead of its old submitted geometry");
    Require(app.GetRenderStats().swap_successes > before_move.swap_successes,
            "Moved target never reached a successful pixel submission");

    pointer.Click(old_x, y);
    Stable(app, "snapshot-retired-old-position");
    Require(observer.actions.size() == 2, "New input still used retired target geometry");
    pointer.Click(new_x, y);
    WaitForActions(app, observer, 3, "activate");
    Stable(app, "snapshot-current-new-position");
    Require(observer.actions == std::vector<std::string>({"activate", "activate", "activate"}),
            "New input did not use the moved and shrunk target geometry");

    // The first queued click replaces the UI from its action handler. The next
    // click carries the same old UI/snapshot and must never reach the replacement.
    observer.replace_on_action = true;
    pointer.Click(new_x, y);
    pointer.Click(new_x, y);
    HoldUiForWorker();
    WaitForActions(app, observer, 4, "activate");
    Stable(app, "snapshot-replacement-drops-queued-input");
    Require(!observer.replace_on_action &&
                observer.actions ==
                    std::vector<std::string>({"activate", "activate", "activate", "activate"}),
            "Old UI input reached the replacement or activation failed");
    pointer.Click(new_x, y);
    WaitForActions(app, observer, 5, "replacement");
    Stable(app, "snapshot-replacement-accepts-current-input");
    Require(observer.actions.size() == 5 && observer.actions.back() == "replacement",
            "Replacement did not accept input after its own successful submission");

    CheckGestures(app, pointer, observer, new_x, y);
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: sdk_input_snapshot_probe <socket>\n";
        return 2;
    }
    try {
        return Verify(argv[1]);
    } catch (const std::exception &error) {
        std::cerr << "SDK input snapshot probe failed: " << error.what() << '\n';
        return 1;
    }
}
