#include "prism/sdk/client_application.hpp"
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>

namespace {
using Application = prism::sdk::ClientApplication;
using Stats = prism::sdk::ClientRenderStats;
using namespace std::chrono_literals;

void Require(bool condition, const char *detail)
{
    if (!condition) {
        throw std::runtime_error(detail);
    }
}

void Print(std::string_view scenario, const Stats &stats)
{
    std::cout << "scenario=" << scenario << " build_calls=" << stats.scene_build_attempts
              << " builds=" << stats.scene_builds << " layouts=" << stats.scene_layouts
              << " gpu_attempts=" << stats.gpu_render_attempts
              << " gpu_successes=" << stats.gpu_render_successes
              << " swap_attempts=" << stats.swap_attempts
              << " swap_successes=" << stats.swap_successes
              << " frame_done=" << stats.frame_callbacks_done
              << " state=" << stats.surface_state_commits
              << " pixels=" << stats.surface_pixel_commits << " noops=" << stats.surface_noops
              << " failures=" << stats.surface_submission_failures
              << " full_repairs=" << stats.full_pixel_repairs
              << " partial_repairs=" << stats.partial_pixel_repairs
              << " empty_repairs=" << stats.empty_pixel_repairs
              << " repair_pixels=" << stats.pixel_repair_pixels
              << " content_pixels=" << stats.content_damage_pixels
              << " history_commits=" << stats.damage_history_commits
              << " age=" << stats.last_buffer_age << " age_supported=" << stats.buffer_age_supported
              << " swap_damage_supported=" << stats.swap_damage_supported
              << " partial_update_supported=" << stats.partial_update_supported << '\n'
              << std::flush;
}

void Until(Application &app, std::function<bool()> condition, const char *detail)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!condition()) {
        Require(std::chrono::steady_clock::now() < deadline, detail);
        Require(app.Pump(10), "client stopped while waiting for submission");
    }
}

struct ActivitySnapshot {
    int configure_count{};
    int presentation_count{};
    bool frame_callback_pending{};
    std::uint64_t frame_callbacks_done{};
    std::uint64_t swap_successes{};
    std::uint64_t surface_state_commits{};
    std::uint64_t surface_pixel_commits{};

    bool operator==(const ActivitySnapshot &) const = default;
};

ActivitySnapshot ObserveActivity(const Application &app)
{
    const auto stats = app.GetRenderStats();
    return {app.ConfigureCount(),       app.PresentationCount(), app.FrameCallbackPending(),
            stats.frame_callbacks_done, stats.swap_successes,    stats.surface_state_commits,
            stats.surface_pixel_commits};
}

void Drain(Application &app)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    auto quiet_since = std::chrono::steady_clock::now();
    auto previous = ObserveActivity(app);

    while (std::chrono::steady_clock::now() < deadline) {
        Require(app.Pump(20), "client stopped while draining events");

        const auto now = std::chrono::steady_clock::now();
        const auto current = ObserveActivity(app);
        if (current != previous) {
            previous = current;
            quiet_since = now;
        }
        if (!current.frame_callback_pending &&
            (!app.HasPresentationFeedback() || current.presentation_count > 0) &&
            now - quiet_since >= 200ms) {
            return;
        }
    }

    throw std::runtime_error("client did not settle after configure/pixel/state activity");
}

void NoPixels(const Stats &before, const Stats &after, const char *detail)
{
    Require(after.gpu_render_attempts == before.gpu_render_attempts &&
                after.swap_attempts == before.swap_attempts &&
                after.surface_pixel_commits == before.surface_pixel_commits,
            detail);
}

void Quiet(Application &app, std::string_view scenario)
{
    Drain(app);
    const auto before = app.GetRenderStats();
    const auto deadline = std::chrono::steady_clock::now() + 350ms;
    while (std::chrono::steady_clock::now() < deadline) {
        Require(app.Pump(25), "static client stopped");
    }
    const auto after = app.GetRenderStats();
    Print(scenario, after);
    NoPixels(before, after, "static/no-op work rendered or swapped pixels");
    Require(after.scene_builds == before.scene_builds &&
                after.scene_layouts == before.scene_layouts &&
                after.surface_state_commits == before.surface_state_commits,
            "static client rebuilt or recommitted state");
}

prism::contracts::ThemeSnapshot ProbeTheme()
{
    prism::contracts::ThemeSnapshot theme;
    theme.id = "submission-probe";
    theme.name = "Submission probe";
    theme.generation = 1;
    theme.colors = {{"foreground", {235, 240, 250, 255}}};
    return theme;
}

int Verify(const std::string &socket, const std::string &app_id)
{
    const auto config = [&](std::string id) {
        return prism::sdk::ClientConfig{socket,
                                        std::move(id),
                                        "SDK submission verification",
                                        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                        640,
                                        400};
    };
    Application app(config(app_id));
    auto theme = ProbeTheme();
    Require(app.ApplyTheme(theme), "initial theme rejected");
    Require(app.Open(R"(
        Card(backdropBlur:$blur,inputShape:$shape,background:$tint,clip:true,cornerRadius:12,padding:20) {
            Text($title,font:20,foreground:"@foreground")
        }
    )"),
            "probe UI failed to open");
    Require(app.SetBinding("blur", 12.0), "blur binding rejected");
    Require(app.SetBinding("shape", std::string("bounds")), "input-shape binding rejected");
    Require(app.SetBinding("tint", prism::contracts::Color{30, 40, 55, 180}),
            "tint binding rejected");
    Require(app.SetBinding("title", std::string("Initial pixels")), "title binding rejected");
    Until(
        app, [&] { return app.IsMapped() && app.GetRenderStats().swap_successes > 0; },
        "first pixels missing");
    Require(app.GlRenderer().find("V3D") != std::string::npos, "probe did not use V3D");
    Drain(app);
    Print("first-pixels", app.GetRenderStats());
    Quiet(app, "idle");
    const auto same_before = app.GetRenderStats();
    for (int i = 0; i < 20; ++i) {
        Require(app.SetBinding("title", std::string("Initial pixels")),
                "same accepted binding rejected");
        Require(app.SetBinding("blur", 12.0), "same blur binding rejected");
    }
    Require(app.ApplyTheme(theme), "same theme rejected");
    Require(app.Pump(0), "no-op pump failed");
    NoPixels(same_before, app.GetRenderStats(), "same value/theme rendered pixels");
    Quiet(app, "same-value");
    auto before = app.GetRenderStats();
    Require(app.SetBinding("blur", 18.0), "metadata binding rejected");
    Until(
        app,
        [&] { return app.GetRenderStats().surface_state_commits > before.surface_state_commits; },
        "effect state was not committed");
    auto after = app.GetRenderStats();
    Print("state-only", after);
    NoPixels(before, after, "effect-only change rendered or swapped pixels");
    Require(after.scene_builds == before.scene_builds &&
                after.scene_layouts == before.scene_layouts,
            "effect-only change rebuilt draw commands");
    // Request Pixels and stop as soon as submitted, before pumping its callback.
    before = app.GetRenderStats();
    Require(app.SetBinding("title", std::string("Pending pixels")), "pixel binding rejected");
    Until(
        app, [&] { return app.GetRenderStats().swap_successes > before.swap_successes; },
        "pixel update missing");
    before = app.GetRenderStats();
    Require(app.SetBinding("blur", 22.0), "pending metadata binding rejected");
    Until(
        app,
        [&] { return app.GetRenderStats().surface_state_commits > before.surface_state_commits; },
        "state after pixel submission missing");
    after = app.GetRenderStats();
    Print("state-after-pixel-submission", after);
    // The worker may consume the pixel callback before the UI observes its
    // status. wayland_submit_test covers a deliberately withheld callback.
    NoPixels(before, after, "state during callback rendered pixels");
    Drain(app);
    before = app.GetRenderStats();
    Require(app.SetBinding("tint", prism::contracts::Color{55, 35, 30, 180}),
            "recovery tint rejected");
    Until(
        app, [&] { return app.GetRenderStats().swap_successes > before.swap_successes; },
        "pixels did not resume after state commit");
    Drain(app);
    Print("pixels-after-metadata", app.GetRenderStats());
    before = app.GetRenderStats();
    auto identity = theme;
    identity.generation = 2;
    identity.id = "same-style";
    identity.name = "Same style";
    Require(app.ApplyTheme(identity), "identity-only theme rejected");
    Require(app.ThemeGeneration() == 2 && app.Pump(0), "theme identity was not retained");
    NoPixels(before, app.GetRenderStats(), "theme identity-only change rendered pixels");
    auto invalid = identity;
    invalid.generation = 3;
    invalid.colors.clear();
    std::string diagnostic;
    Require(!app.ApplyTheme(invalid, &diagnostic) && !diagnostic.empty(),
            "missing theme reference was accepted");
    Require(app.ThemeGeneration() == 2, "failed preflight changed current theme");
    Require(!app.ReplaceUi("UnsupportedWidget()"), "invalid replacement UI was accepted");
    Require(app.Pump(0), "preflight rejection stopped the existing UI");
    NoPixels(before, app.GetRenderStats(), "failed preflight submitted pixels");
    Quiet(app, "preflight-rejection");
    // A second ordinary production client changes the first client's BSP size.
    // This exercises real configure/resize without embedding WM code here.
    const int configured = app.ConfigureCount();
    before = app.GetRenderStats();
    Application peer(config(app_id + ".resize-peer"));
    Require(peer.Open("Card(background:#223344FF) { Text(\"Resize peer\") }"),
            "resize peer failed to open");
    const auto resize_deadline = std::chrono::steady_clock::now() + 5s;
    while (!peer.IsMapped() || app.ConfigureCount() == configured ||
           app.GetRenderStats().swap_successes == before.swap_successes) {
        Require(std::chrono::steady_clock::now() < resize_deadline,
                "production BSP resize did not produce pixels");
        Require(peer.Pump(5) && app.Pump(5), "resize client stopped");
    }
    Drain(app);
    Print("bsp-resize", app.GetRenderStats());
    before = app.GetRenderStats();
    peer.Close();
    Until(
        app, [&] { return app.GetRenderStats().swap_successes > before.swap_successes; },
        "removing peer did not restore pixel size");
    Drain(app);
    Quiet(app, "resize-restored");
    // Two successive external wakes prove interruptible indefinite waiting and
    // a drained notification, without using periodic polling as a runtime wake.
    const int wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    Require(wake >= 0, "wake eventfd failed");
    for (int round = 0; round < 2; ++round) {
        before = app.GetRenderStats();
        pollfd descriptor{wake, POLLIN, 0};
        const auto started = std::chrono::steady_clock::now();
        std::thread notify([&] {
            std::this_thread::sleep_for(120ms);
            const std::uint64_t one = 1;
            Require(write(wake, &one, sizeof(one)) == sizeof(one), "wake write failed");
        });
        unsigned pumps = 0;
        while (!(descriptor.revents & POLLIN)) {
            Require(++pumps <= 4, "indefinite wait woke repeatedly without work");
            Require(app.Pump(-1, std::span(&descriptor, 1)), "external wake pump failed");
        }
        notify.join();
        Require(std::chrono::steady_clock::now() - started >= 70ms, "idle runtime failed to block");
        std::uint64_t value{};
        Require(read(wake, &value, sizeof(value)) == sizeof(value) && value == 1,
                "wake did not drain once");
        NoPixels(before, app.GetRenderStats(), "external wake submitted pixels");
    }
    close(wake);
    Print("interruptible-wait", app.GetRenderStats());
    app.Close();
    return 0;
}

int VerifyDamage(const std::string &socket, const std::string &app_id)
{
    // Actual Wayland/EGL ages here; rotating-memory age simulation and full
    // target pixel equality live in the independent skia_damage_test.
    constexpr std::string_view ui = R"(
        VStack(background:#192A3BB0,padding:24,spacing:12,clip:true,cornerRadius:14) {
            Text("Pixel repair",font:22,foreground:#F0F4FFFF)
            Text($tick,height:28,font:18,foreground:#EE7799FF)
            Progress(value:$progress,height:8,cornerRadius:4,foreground:#EE7799FF,background:#64748B70)
            VStack(flex:1) {}
            Icon("music",width:48,height:48,foreground:#EE7799FF)
        }
    )";
    for (bool partial : {true, false}) {
        prism::sdk::ClientConfig config{socket,
                                        app_id + (partial ? ".auto" : ".full"),
                                        "Buffer damage verification",
                                        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                        640,
                                        400};
        config.partial_rendering = partial;
        Application app(config);
        Require(app.Open(ui), "damage UI failed to open");
        Require(app.SetBinding("tick", std::string("0000")) && app.SetBinding("progress", 0.2),
                "initial damage bindings failed");
        Until(app, [&] { return app.IsMapped(); }, "damage client did not map");
        Drain(app);
        Require(app.GlRenderer().find("V3D") != std::string::npos,
                "damage verification requires V3D");
        const auto before = app.GetRenderStats();
        Print(partial ? "damage-auto-start" : "damage-full-start", before);
        for (int i = 1; i <= 24; ++i) {
            const auto frame = app.GetRenderStats();
            const auto digits = std::string("000") + std::to_string(i % 10);
            Require(app.SetBinding("tick", digits), "tick binding failed");
            Require(app.SetBinding("progress", 0.2 + (i % 8) * 0.025), "progress binding failed");
            Until(
                app, [&] { return app.GetRenderStats().swap_successes > frame.swap_successes; },
                "damage pixels did not swap");
            Drain(app);
        }
        const auto after = app.GetRenderStats();
        Print(partial ? "damage-auto" : "damage-full", after);
        Require(after.damage_history_commits == after.swap_successes &&
                    after.surface_submission_failures == 0,
                "successful content history or submission failed");
        Require(after.swap_successes - before.swap_successes == 24,
                "pixel updates were coalesced/lost");
        if (partial && after.buffer_age_supported) {
            Require(after.partial_pixel_repairs > before.partial_pixel_repairs,
                    "actual buffer ages never enabled partial repair");
        }
        if (!partial) {
            Require(after.partial_pixel_repairs == 0 &&
                        after.full_pixel_repairs == after.gpu_render_successes,
                    "full repair policy unexpectedly drew partial pixels");
        }
        const auto no_op = app.GetRenderStats();
        Require(app.SetBinding("tick", std::string("0004")) && app.Pump(0),
                "same-value damage binding failed");
        NoPixels(no_op, app.GetRenderStats(), "repeated binding rendered damage pixels");
        Quiet(app, partial ? "damage-auto-idle" : "damage-full-idle");
        app.Close();
    }
    return 0;
}

int VerifyAnimation(const std::string &socket, const std::string &app_id)
{
    Application app({socket, app_id, "Animation verification",
                     "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 640, 400});
    Require(app.Open(R"(
        Card(background:#192A3BFF,padding:24,visible:$show) {
            Progress(value:$progress,height:10,foreground:#EE7799FF,background:#64748B70)
                .transition(property:"value",durationMs:600,easing:"easeOutCubic")
        }
    )"),
            "animation UI failed to open");
    Until(app, [&] { return app.IsMapped(); }, "animation client did not map");
    Drain(app);
    Require(app.GlRenderer().find("V3D") != std::string::npos,
            "animation verification requires V3D");
    Quiet(app, "animation-initial-idle");

    const auto before = app.GetRenderStats();
    Require(app.SetBinding("progress", 0.8), "animated progress binding failed");
    Until(
        app, [&] { return app.GetRenderStats().swap_successes >= before.swap_successes + 2; },
        "animation did not produce multiple paced frames");
    Require(app.SetBinding("progress", 0.1), "animation retarget binding failed");

    const auto settle = std::chrono::steady_clock::now() + 900ms;
    while (std::chrono::steady_clock::now() < settle) {
        Require(app.Pump(20), "animation client stopped before the final sample");
    }
    Drain(app);
    const auto after = app.GetRenderStats();
    Print("animation-retarget-finished", after);
    Require(after.swap_successes > before.swap_successes + 2 &&
                after.scene_layouts == before.scene_layouts &&
                after.surface_submission_failures == 0,
            "animation lost frames, recomputed layout or failed submission");
    Quiet(app, "animation-finished-idle");

    const auto before_hide = app.GetRenderStats();
    Require(app.SetBinding("progress", 0.8), "second animation binding failed");
    Until(
        app, [&] { return app.GetRenderStats().swap_successes >= before_hide.swap_successes + 2; },
        "second animation did not start");
    Require(app.SetBinding("show", false), "animation hide binding failed");
    Drain(app);
    Quiet(app, "animation-hidden-idle");
    Require(app.SetBinding("show", true), "animation reveal binding failed");
    Drain(app);
    Quiet(app, "animation-revealed-idle");
    app.Close();
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::cerr << "usage: prism_skia_gles_wayland_probe <socket> "
                     "<dsl-file|--verify-submission|--verify-damage|--verify-animation> [app-id]\n";
        return 2;
    }
    try {
        if (std::string_view(argv[2]) == "--verify-submission") {
            return Verify(argv[1], argc > 3 ? argv[3] : "prism.skia.submission.probe");
        }
        if (std::string_view(argv[2]) == "--verify-damage") {
            return VerifyDamage(argv[1], argc > 3 ? argv[3] : "prism.skia.damage.probe");
        }
        if (std::string_view(argv[2]) == "--verify-animation") {
            return VerifyAnimation(argv[1], argc > 3 ? argv[3] : "prism.skia.animation.probe");
        }
        std::ifstream input(argv[2]);
        if (!input) {
            return 2;
        }
        std::string source(std::istreambuf_iterator<char>{input}, {});
        Application app({argv[1], argc > 3 ? argv[3] : "prism.skia.gles.probe",
                         "Prism Skia GLES DSL", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                         640, 400});
        app.OnAction([](std::string_view action) { std::cout << "action=" << action << '\n'; });
        if (!app.Open(source)) {
            return 4;
        }
        for (int i = 0; i < 500 && !app.IsCloseRequested(); ++i) {
            if (!app.Pump(20)) {
                break;
            }
            if (i == 120) {
                app.SetSlot("title", "Skia GLES + Wayland");
            }
        }
        std::cout << "GL renderer=" << app.GlRenderer() << "\nconfigure=" << app.ConfigureCount()
                  << " frame=" << app.FrameDoneCount() << " presented=" << app.PresentedCount()
                  << " images=" << app.LoadedImageCount() << '/' << app.RequestedImageCount()
                  << '\n';
        Print("final", app.GetRenderStats());
        return app.GlRenderer().find("V3D") != std::string::npos && app.IsMapped() &&
                       app.FrameDoneCount() > 0 && app.PresentedCount() > 0 &&
                       app.LoadedImageCount() == app.RequestedImageCount()
                   ? 0
                   : 5;
    } catch (const std::exception &error) {
        std::cerr << "GLES submission probe failed: " << error.what() << '\n';
        return 6;
    }
}
