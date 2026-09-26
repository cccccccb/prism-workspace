#include "dock_item.hpp"
#include "prism/sdk/client_application.hpp"
#include <string>
#include <signal.h>

int main() {
    signal(SIGCHLD, SIG_IGN); // Reap successfully spawned clients without zombies.
    auto source = prism::sdk::LoadUiSource("dock.prism", "prism-dock/ui/dock.prism");
    if (!source) return 2;
    prism::sdk::ClientApplication app({{}, "prism_dock", "Prism Dock",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 800, 72});
    if (!app.Open(*source)) return 1;
    prism::dock::DockManager dock;
    app.SetSlot("running_badge", "0 Active");
    app.OnAction([&](std::string_view action) {
        constexpr std::string_view prefix = "app:launch:";
        if (!action.starts_with(prefix)) return;
        if (dock.LaunchOrActivate(std::string(action.substr(prefix.size()))))
            app.SetSlot("running_badge", std::to_string(dock.GetRunningCount()) + " Active");
    });
    while (!app.IsCloseRequested()) if (!app.Pump(100)) return app.IsCloseRequested() ? 0 : 1;
    return 0;
}
