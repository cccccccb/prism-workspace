#include "prism/sdk/client_application.hpp"
#include <chrono>
#include <string>

int main() {
    auto source = prism::sdk::LoadUiSource("demo_settings.prism", "demos/demo_settings/master.prism");
    if (!source) return 2;
    prism::sdk::ClientApplication app({{}, "demo_settings", "Prism System Preferences",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 960, 720});
    if (!app.Open(*source)) return 1;
    bool dark = true;
    double cpu = 0.18;
    app.SetSlot("dark_mode_btn", "Theme: Dark");
    app.SetSlot("mem_usage_text", "Unified Memory: 4.2 / 32 GB (13%)");
    app.OnAction([&](std::string_view action) {
        if (action == "theme:toggle") {
            dark = !dark;
            app.SetSlot("dark_mode_btn", dark ? "Theme: Dark" : "Theme: Light");
        } else if (action == "sys:purge") {
            app.SetSlot("mem_usage_text", "Unified Memory: 2.6 / 32 GB (8%) [Purged]");
        }
    });
    auto last_tick = std::chrono::steady_clock::now();
    while (!app.IsCloseRequested()) {
        if (!app.Pump(100)) return app.IsCloseRequested() ? 0 : 1;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_tick < std::chrono::milliseconds(400)) continue;
        last_tick = now;
        cpu = cpu > 0.85 ? 0.15 : cpu + 0.03;
        app.SetSlot("cpu_usage_text", "CPU Load: " + std::to_string(static_cast<int>(cpu * 100)) + "%");
    }
    return 0;
}
