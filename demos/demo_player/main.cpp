#include "prism/sdk/client_application.hpp"
#include <chrono>
#include <string>

int main() {
    auto source = prism::sdk::LoadUiSource("demo_player.prism", "demos/demo_player/master.prism");
    if (!source) return 2;
    prism::sdk::ClientApplication app({{}, "demo_player", "Prism Music Studio",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 960, 720});
    if (!app.Open(*source)) return 1;
    bool playing = false;
    double progress = 0.42;
    app.SetSlot("track_title", "Hotel California - Eagles");
    app.SetSlot("play_state_icon", "Play");
    app.OnAction([&](std::string_view action) {
        if (action == "player:toggle") {
            playing = !playing;
            app.SetSlot("play_state_icon", playing ? "Pause" : "Play");
        } else if (action == "nav:favorites") {
            app.SetSlot("track_title", "Starboy - The Weeknd");
            progress = 0;
        } else if (action == "nav:library") {
            app.SetSlot("track_title", "Bohemian Rhapsody - Queen");
            progress = 0.15;
        }
    });
    auto last_tick = std::chrono::steady_clock::now();
    while (!app.IsCloseRequested()) {
        if (!app.Pump(100)) return app.IsCloseRequested() ? 0 : 1;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_tick < std::chrono::milliseconds(500)) continue;
        last_tick = now;
        if (playing) progress = progress + 0.02 > 1.0 ? 0.0 : progress + 0.02;
        app.SetSlot("playback_progress_text", "Progress: " + std::to_string(static_cast<int>(progress * 100)) + "%");
    }
    return 0;
}
