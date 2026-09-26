#include "prism/sdk/client_application.hpp"
#include <iostream>

int main() {
    auto source = prism::sdk::LoadUiSource("desktop.prism", "prism-desktop/ui/desktop.prism");
    if (!source) return 2;
    prism::sdk::ClientApplication app({{}, "prism_desktop", "Prism Desktop",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 1280, 720});
    if (!app.Open(*source)) return 1;
    app.SetSlot("wallpaper_title", "Prism Desktop");
    while (!app.IsCloseRequested()) if (!app.Pump(100)) return app.IsCloseRequested() ? 0 : 1;
    return 0;
}
