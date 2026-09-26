#include "prism/platform/wayland_window.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    const std::string socket = argc > 1 ? argv[1] : "wayland-prism-0";
    prism::platform::WaylandWindow window;
    window.SetPaintHandler([](void* data, int width, int height, int stride) {
        auto* pixels = static_cast<std::uint32_t*>(data);
        const int pitch = stride / 4;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                pixels[y * pitch + x] =
                    (static_cast<std::uint32_t>(x % 256) << 16) |
                    (static_cast<std::uint32_t>(y % 256) << 8) | 0x70;
            }
        }
    });
    if (!window.Open(socket, "prism.lifecycle-probe", "Prism Wayland Lifecycle Probe", 640, 400)) {
        std::fprintf(stderr, "probe: failed to connect or create xdg-shell window\n");
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    bool maximize_requested = false;
    while (std::chrono::steady_clock::now() < deadline && !window.IsCloseRequested()) {
        if (!window.Pump(100)) break;
        if (window.IsMapped() && window.FrameDoneCount() > 0 && !maximize_requested) {
            window.RequestMaximize();
            maximize_requested = true;
        }
        if (maximize_requested && window.ConfigureCount() >= 2 && window.FrameDoneCount() >= 2)
            break;
    }
    const auto metrics = window.Metrics();
    std::printf("probe: configured=%d mapped=%d configure_count=%d frame_done=%d "
                "size=%.0fx%.0f pointer_enter=%d buttons=%d keys=%d close=%d\n",
                window.IsConfigured(), window.IsMapped(), window.ConfigureCount(),
                window.FrameDoneCount(), metrics.logical_size.width, metrics.logical_size.height,
                window.PointerEnterCount(), window.PointerButtonCount(), window.KeyCount(),
                window.IsCloseRequested());
    return window.IsConfigured() && window.IsMapped() && window.FrameDoneCount() > 0 ? 0 : 1;
}
