#include "prism/sdk/application.hpp"
#include "prism/core/logging.hpp"
#include <thread>
#include <chrono>

int main(int argc, char* argv[]) {
    std::string channel = (argc > 1) ? argv[1] : "/prism_demo_settings";
    std::string pkg = (argc > 2) ? argv[2] : "demos/demo_settings.prismpkg";
    PRISM_LOG_INFO("SETTINGS", "Starting Prism System Preferences & Monitor (Wayland Client Native)...");

    prism::sdk::AppConfig config{};
    config.app_id = "demo_settings";
    config.package_path = pkg;
    config.channel_name = channel;
    config.width = 960;
    config.height = 1080;

    auto app = prism::sdk::Application::Create(config);
    if (!app) {
        PRISM_LOG_ERROR("SETTINGS", "Failed to connect to Prism Compositor!");
        return 1;
    }

    bool dark_mode = true;
    double mem_usage = 0.13;

    app->On("theme:toggle", [&](const prism::ipc::EventPacket&) {
        dark_mode = !dark_mode;
        PRISM_LOG_INFO("SETTINGS", "Theme toggled -> %s", dark_mode ? "Dark Acrylic" : "Light Mica");
        app->SetState("dark_mode_btn", dark_mode ? "Theme: Dark Acrylic" : "Theme: Light Mica");
    });

    app->On("sys:purge", [&](const prism::ipc::EventPacket&) {
        PRISM_LOG_INFO("SETTINGS", "Purging unified memory cache...");
        mem_usage = 0.08;
        app->SetState("mem_usage_text", "Unified Memory: 2.6 / 32 GB (8%) [Purged]");
        app->SetState("mem_usage_val", mem_usage);
    });

    // Populate initial state
    app->SetState("cpu_usage_text", "Apple M-Style Engine: 18% Load");
    app->SetState("cpu_usage_val", 0.18);
    app->SetState("mem_usage_text", "Unified Memory: 4.2 / 32 GB (13%)");
    app->SetState("mem_usage_val", mem_usage);
    app->SetState("dark_mode_btn", "Theme: Dark Acrylic");

    // Announce readiness to WM (triggers 0ms preview -> master morph)
    app->Ready();

    // Background worker simulating real-time system metric polling
    std::thread monitor_thread([&]() {
        double cpu = 0.18;
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            cpu += 0.03;
            if (cpu > 0.85) cpu = 0.15;

            app->SetState("cpu_usage_val", cpu);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "CPU Load: %d%% (8 Cores Active)", static_cast<int>(cpu * 100));
            app->SetState("cpu_usage_text", buf);
        }
    });
    monitor_thread.detach();

    return app->Exec();
}
