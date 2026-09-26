#include "status_manager.hpp"
#include "prism/sdk/client_application.hpp"
#include <chrono>
#include <iostream>

int main() {
    auto source = prism::sdk::LoadUiSource("topbar.prism", "prism-topbar/ui/topbar.prism");
    if (!source) return 2;
    prism::sdk::ClientApplication app({{}, "prism_topbar", "Prism TopBar",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 1280, 38});
    if (!app.Open(*source)) return 1;
    prism::topbar::StatusManager status;
    app.SetSlot("clock_time", status.GetStatus().time_str);
    app.SetSlot("net_status", status.GetStatus().network_str);
    app.SetSlot("bat_status", status.GetStatus().battery_str);
    app.OnAction([&](std::string_view action) {
        if (action == "notifications:toggle") status.ToggleNotifications();
    });
    auto last_tick = std::chrono::steady_clock::now();
    while (!app.IsCloseRequested()) {
        if (!app.Pump(100)) return app.IsCloseRequested() ? 0 : 1;
        auto now = std::chrono::steady_clock::now();
        if (now - last_tick < std::chrono::seconds(1)) continue;
        last_tick = now;
        status.Update();
        app.SetSlot("clock_time", status.GetStatus().time_str);
        app.SetSlot("bat_status", status.GetStatus().battery_str);
    }
    return 0;
}
