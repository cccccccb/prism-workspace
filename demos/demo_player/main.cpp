#include "prism/sdk/application.hpp"
#include "prism/core/logging.hpp"
#include <thread>
#include <chrono>

int main(int argc, char* argv[]) {
    std::string channel = (argc > 1) ? argv[1] : "/prism_demo_player";
    std::string pkg = (argc > 2) ? argv[2] : "demos/demo_player.prismpkg";
    PRISM_LOG_INFO("DEMO", "Starting Prism Music Studio (Wayland Client Native)...");

    prism::sdk::AppConfig config{};
    config.app_id = "demo_player";
    config.package_path = pkg;
    config.channel_name = channel;
    config.width = 960;
    config.height = 1080;

    auto app = prism::sdk::Application::Create(config);
    if (!app) {
        PRISM_LOG_ERROR("DEMO", "Failed to connect to Prism Compositor!");
        return 1;
    }

    bool is_playing = false;

    // Register Observer callbacks for UI actions
    app->On("player:toggle", [&](const prism::ipc::EventPacket&) {
        is_playing = !is_playing;
        PRISM_LOG_INFO("DEMO", "Playback toggled -> %s", is_playing ? "PLAYING" : "PAUSED");
        app->SetState("play_state_icon", is_playing ? "icon.pause" : "icon.play");
    });

    app->On("nav:favorites", [&](const prism::ipc::EventPacket&) {
        PRISM_LOG_INFO("DEMO", "Navigated to Favorites playlist");
        app->SetState("track_title", "Starboy - The Weeknd");
        app->SetState("playback_progress", 0.0);
    });

    app->On("nav:library", [&](const prism::ipc::EventPacket&) {
        PRISM_LOG_INFO("DEMO", "Navigated to Media Library");
        app->SetState("track_title", "Bohemian Rhapsody - Queen");
        app->SetState("playback_progress", 0.15);
    });

    // Simulate backend initialization (loading local database, warming audio decode buffers)
    PRISM_LOG_INFO("DEMO", "Warming up audio decoders and fetching metadata...");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Populate initial reactive slots
    app->SetState("track_title", "Hotel California - Eagles");
    app->SetState("play_state_icon", "icon.play");
    app->SetState("playback_progress", 0.42);

    // Announce backend is ready! (Triggers Preview -> Master crossfade in WM)
    app->Ready();

    // Background playback timer thread
    std::thread timer_thread([&]() {
        double progress = 0.42;
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (is_playing) {
                progress += 0.02;
                if (progress > 1.0) progress = 0.0;
                app->SetState("playback_progress", progress);
            }
        }
    });
    timer_thread.detach();

    return app->Exec();
}
