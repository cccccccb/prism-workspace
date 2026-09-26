#include "prism/platform/wayland_window.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <variant>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: prism_skia_wayland_probe <socket> <dsl-file>\n";
        return 2;
    }
    std::ifstream input(argv[2]);
    if (!input) return 2;
    std::string source(std::istreambuf_iterator<char>{input}, {});
    prism::render_skia::RasterRenderer renderer("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    if (!renderer.Ready()) return 3;
    prism::runtime::Scene scene(prism::runtime::ParseBlueprint(source),
        [&](std::string_view text, double size) { return renderer.Shape(text, size); }, renderer.FontId());
    std::optional<prism::contracts::DisplayList> last_list;
    prism::platform::WaylandWindow window;
    window.SetEventHandler([&](const prism::contracts::WindowEvent& event) {
        if (auto* configured = std::get_if<prism::contracts::ConfigureEvent>(&event))
            scene.SetViewport(configured->metrics.logical_size);
        else if (auto* button = std::get_if<prism::contracts::PointerButtonEvent>(&event)) {
            if (button->state == prism::contracts::ButtonState::Pressed) {
                if (auto action = scene.ActionAt(button->position))
                    std::cout << "action=" << *action << '\n';
            }
        }
    });
    window.SetPaintHandler([&](void* pixels, int width, int height, int stride) {
        if (auto next = scene.Build(prism::contracts::WindowId{1})) last_list = std::move(next);
        if (!last_list || !renderer.Render(*last_list, pixels, width, height, stride))
            std::cerr << "Skia render failed\n";
    });
    if (!window.Open(argv[1], "prism.skia.probe", "Prism Skia DSL", 640, 400)) return 4;
    for (int i = 0; i < 500 && !window.IsCloseRequested(); ++i) {
        if (!window.Pump(20)) break;
        if (i == 120) {
            scene.SetSlot("title", "Skia + Wayland");
            window.RequestRedraw();
        }
    }
    std::cout << "configure=" << window.ConfigureCount()
              << " frame=" << window.FrameDoneCount() << '\n';
    return window.IsMapped() && window.FrameDoneCount() > 0 ? 0 : 5;
}
