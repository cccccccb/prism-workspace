#include "prism/platform/wayland_window.hpp"
#include "prism/platform/wayland_egl_surface.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include "prism/render_skia/gles_renderer.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <variant>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: prism_skia_gles_wayland_probe <socket> <dsl-file>\n";
        return 2;
    }
    std::ifstream input(argv[2]);
    if (!input) return 2;
    std::string source(std::istreambuf_iterator<char>{input}, {});
    prism::render_skia::RasterRenderer commands("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    if (!commands.Ready()) return 3;
    prism::runtime::ImageResources resources(prism::render_skia::RasterRenderer::DecodePng);
    int requested_images = 0, loaded_images = 0, presented = 0;
    std::string gl_renderer;
    prism::runtime::Scene scene(prism::runtime::ParseBlueprint(source,
        [&](std::string_view uri) {
            ++requested_images;
            return resources.Request(std::string(uri));
        }),
        [&](std::string_view text, double size) { return commands.Shape(text, size); }, commands.FontId());
    std::optional<prism::contracts::DisplayList> last_list;
    prism::platform::WaylandEglSurface egl;
    std::unique_ptr<prism::render_skia::GlesRenderer> renderer;
    prism::platform::WaylandWindow window;
    window.SetEventHandler([&](const prism::contracts::WindowEvent& event) {
        if (auto* configured = std::get_if<prism::contracts::ConfigureEvent>(&event))
            scene.SetViewport(configured->metrics.logical_size);
    });
    window.SetPresentHandler([&](wl_display* display, wl_surface* surface, int width, int height) {
        if (!egl.Ready()) {
            if (!egl.Open(display, surface, width, height)) {
                std::cerr << "EGL open failed\n";
                return false;
            }
            gl_renderer = egl.GlRenderer();
            std::cout << "GL vendor=" << egl.GlVendor() << " renderer=" << gl_renderer
                      << " version=" << egl.GlVersion() << '\n';
            renderer = std::make_unique<prism::render_skia::GlesRenderer>(commands);
        }
        if (!renderer || !renderer->Ready() || !egl.Resize(width, height) || !egl.MakeCurrent())
            return false;
        if (auto next = scene.Build(prism::contracts::WindowId{1})) last_list = std::move(next);
        if (!last_list || !renderer->Render(*last_list, width, height) || !egl.Swap()) {
            std::cerr << "Skia GLES render/present failed\n";
            return false;
        }
        ++presented;
        return true;
    });
    if (!window.Open(argv[1], "prism.skia.gles.probe", "Prism Skia GLES DSL", 640, 400)) return 4;
    for (int i = 0; i < 500 && !window.IsCloseRequested(); ++i) {
        if (!window.Pump(20)) break;
        for (const auto& update : resources.Poll()) {
            if (update.state == prism::runtime::ImageState::Ready) {
                if (const auto* image = resources.Get(update.id)) {
                    if (commands.RegisterImage(update.id, *image) &&
                        scene.ImageReady(update.id, update.intrinsic_size)) {
                        ++loaded_images;
                        window.RequestRedraw();
                    }
                }
            } else std::cerr << "image load failed, id=" << update.id.value << '\n';
        }
        if (i == 120) {
            scene.SetSlot("title", "Skia GLES + Wayland");
            window.RequestRedraw();
        }
    }
    const bool okay = gl_renderer.find("V3D") != std::string::npos &&
                      window.IsMapped() && window.FrameDoneCount() > 0 && presented > 0 &&
                      loaded_images == requested_images;
    std::cout << "configure=" << window.ConfigureCount() << " frame=" << window.FrameDoneCount()
              << " presented=" << presented << " images=" << loaded_images << '/'
              << requested_images << '\n';
    renderer.reset();
    egl.Close();
    window.Close();
    return okay ? 0 : 5;
}
