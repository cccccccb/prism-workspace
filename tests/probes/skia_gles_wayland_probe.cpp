#include "prism/sdk/client_application.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: prism_skia_gles_wayland_probe <socket> <dsl-file>\n";
        return 2;
    }
    std::ifstream input(argv[2]);
    if (!input) return 2;
    std::string source(std::istreambuf_iterator<char>{input}, {});
    prism::sdk::ClientApplication app({argv[1], argc > 3 ? argv[3] : "prism.skia.gles.probe", "Prism Skia GLES DSL",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 640, 400});
    app.OnAction([](std::string_view action) { std::cout << "action=" << action << '\n'; });
    if (!app.Open(source)) return 4;
    for (int i = 0; i < 500 && !app.IsCloseRequested(); ++i) {
        if (!app.Pump(20)) break;
        if (i == 120) app.SetSlot("title", "Skia GLES + Wayland");
    }
    std::cout << "GL renderer=" << app.GlRenderer() << '\n'
              << "configure=" << app.ConfigureCount() << " frame=" << app.FrameDoneCount()
              << " presented=" << app.PresentedCount() << " images=" << app.LoadedImageCount()
              << '/' << app.RequestedImageCount() << '\n';
    return app.GlRenderer().find("V3D") != std::string::npos && app.IsMapped() &&
           app.FrameDoneCount() > 0 && app.PresentedCount() > 0 &&
           app.LoadedImageCount() == app.RequestedImageCount() ? 0 : 5;
}
