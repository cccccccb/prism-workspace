#include "prism/platform/wayland_window.hpp"
#include "prism/wm/compositor.hpp"
#include "prism/wm/wlr_server.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <type_traits>
#include <variant>
#include <unistd.h>

extern "C" {
#include <wlr/interfaces/wlr_keyboard.h>
}

int main() {
    char runtime_template[] = "/tmp/prism-wayland-test.XXXXXX";
    char* runtime_dir = mkdtemp(runtime_template);
    if (!runtime_dir) return 1;
    setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);

    auto compositor = std::make_shared<prism::wm::Compositor>();
    if (!compositor->Initialize()) return 2;
    prism::wm::WlrServer server(compositor);
    const std::string socket = "wayland-prism-lifecycle-" + std::to_string(getpid());
    if (!server.Initialize(socket)) return 3;
    server.Start();

    wlr_keyboard test_keyboard{};
    static const wlr_keyboard_impl keyboard_impl{.name = "prism-test-keyboard",
                                                  .led_update = nullptr};
    wlr_keyboard_init(&test_keyboard, &keyboard_impl, "prism-test-keyboard");
    server.HandleNewInput(&test_keyboard.base);

    std::atomic<bool> client_mapped{false};
    std::atomic<bool> client_done{false};
    std::atomic<bool> client_pass{false};
    std::thread client([&] {
        prism::platform::WaylandWindow window;
        window.SetPaintHandler([](void* data, int width, int height, int stride) {
            auto* pixels = static_cast<std::uint32_t*>(data);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    pixels[y * (stride / 4) + x] = 0x00336699;
        });
        int configure_events = 0;
        int pointer_events = 0;
        int key_events = 0;
        int close_events = 0;
        window.SetEventHandler([&](const prism::contracts::WindowEvent& event) {
            std::visit([&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, prism::contracts::ConfigureEvent>) {
                    ++configure_events;
                } else if constexpr (std::is_same_v<T, prism::contracts::PointerButtonEvent>) {
                    if (value.button == prism::contracts::PointerButton::Primary) ++pointer_events;
                } else if constexpr (std::is_same_v<T, prism::contracts::KeyEvent>) {
                    if (value.physical_key == 0x04) ++key_events;
                } else if constexpr (std::is_same_v<T, prism::contracts::CloseRequestedEvent>) {
                    ++close_events;
                }
            }, event);
        });
        if (!window.Open(socket, "prism.lifecycle-test", "Lifecycle Test", 640, 400)) {
            client_done = true;
            return;
        }
        bool maximized = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!window.Pump(20) && !window.IsCloseRequested()) break;
            if (window.IsMapped()) client_mapped = true;
            if (!maximized && window.FrameDoneCount() > 0) {
                window.RequestMaximize();
                maximized = true;
            }
            if (window.IsCloseRequested()) break;
        }
        client_pass = window.IsConfigured() && window.IsMapped() &&
                      window.ConfigureCount() >= 2 && window.FrameDoneCount() >= 2 &&
                      window.Metrics().buffer_size.width == 1280 &&
                      window.PointerEnterCount() > 0 && window.PointerButtonCount() >= 2 &&
                      window.KeyCount() >= 2 &&
                      configure_events >= 2 && pointer_events >= 2 &&
                      key_events >= 2 && close_events == 1 &&
                      window.IsCloseRequested();
        std::fprintf(stderr, "lifecycle: configure=%d frame=%d pointer_enter=%d buttons=%d keys=%d close=%d\n",
                     window.ConfigureCount(), window.FrameDoneCount(),
                     window.PointerEnterCount(), window.PointerButtonCount(),
                     window.KeyCount(),
                     window.IsCloseRequested());
        client_done = true;
    });

    bool input_sent = false;
    bool key_sent = false;
    bool close_sent = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(9);
    auto input_time = deadline;
    while (!client_done && std::chrono::steady_clock::now() < deadline) {
        server.RunEventLoopIteration(10);
        if (client_mapped && !input_sent) {
            server.HandleCursorMotionAbsolute(100, 0.25, 0.25);
            server.HandleCursorButton(101, 272, 1);
            server.HandleCursorButton(102, 272, 0);
            input_sent = true;
            input_time = std::chrono::steady_clock::now();
        }
        if (input_sent && !key_sent) {
            wlr_keyboard_key_event pressed{.time_msec = 103, .keycode = 30,
                                           .update_state = true,
                                           .state = WL_KEYBOARD_KEY_STATE_PRESSED};
            wlr_keyboard_notify_key(&test_keyboard, &pressed);
            wlr_keyboard_key_event released{.time_msec = 104, .keycode = 30,
                                            .update_state = true,
                                            .state = WL_KEYBOARD_KEY_STATE_RELEASED};
            wlr_keyboard_notify_key(&test_keyboard, &released);
            key_sent = true;
        }
        if (input_sent && !close_sent &&
            std::chrono::steady_clock::now() - input_time > std::chrono::milliseconds(500)) {
            server.CloseFocusedXdgView();
            close_sent = true;
        }
    }
    if (!client_done) {
        server.Stop();
        client.join();
        wlr_keyboard_finish(&test_keyboard);
        std::filesystem::remove_all(runtime_dir);
        return 4;
    }
    client.join();

    // Two independent xdg clients must receive the WM's split geometry via configure.
    std::atomic<int> tiled_clients{0};
    std::atomic<bool> release_tiled_clients{false};
    auto run_tiled_client = [&](int id) {
        prism::platform::WaylandWindow window;
        window.SetPaintHandler([](void* data, int width, int height, int stride) {
            auto* pixels = static_cast<std::uint32_t*>(data);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    pixels[y * (stride / 4) + x] = 0x00557799;
        });
        if (!window.Open(socket, "prism.tile-" + std::to_string(id),
                         "Tiling Test", 500, 300)) return;
        bool reported = false;
        const auto tiled_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < tiled_deadline && !release_tiled_clients) {
            if (!window.Pump(20)) break;
            const auto metrics = window.Metrics();
            if (!reported && window.IsMapped() && metrics.buffer_size.width == 640 &&
                metrics.buffer_size.height == 614 && window.FrameDoneCount() > 0) {
                ++tiled_clients;
                reported = true;
            }
        }
    };
    std::thread tile_a(run_tiled_client, 1);
    std::thread tile_b(run_tiled_client, 2);
    const auto tiled_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (tiled_clients < 2 && std::chrono::steady_clock::now() < tiled_deadline) {
        server.RunEventLoopIteration(10);
    }
    release_tiled_clients = true;
    if (tiled_clients < 2) server.Stop();
    tile_a.join();
    tile_b.join();
    if (tiled_clients == 2) {
        for (int i = 0; i < 5; ++i) server.RunEventLoopIteration(10);
    }

    wlr_keyboard_finish(&test_keyboard);
    server.Stop();
    std::filesystem::remove_all(runtime_dir);
    std::fprintf(stderr, "lifecycle: tiled_clients=%d\n", tiled_clients.load());
    return client_pass && tiled_clients == 2 ? 0 : 4;
}
