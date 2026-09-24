#include "status_manager.hpp"
#include "prism/sdk/application.hpp"
#include "prism/compiler/binary_generator.hpp"
#include "prism/compiler/lexer.hpp"
#include "prism/compiler/parser.hpp"
#include "prism/core/logging.hpp"
#include <iostream>
#include <fstream>
#include <string>
#include <memory>
#include <filesystem>
#include <thread>
#include <chrono>
#include <atomic>
#include <cassert>

using namespace prism;

int main(int argc, char* argv[]) {
    std::string channel = (argc > 1) ? argv[1] : "/prism_topbar_ipc";

    bool test_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--test" || std::string(argv[i]) == "--auto-exit") {
            test_mode = true;
        }
    }

    PRISM_LOG_INFO("TOPBAR", "Starting Prism-TopBar Shell (Layer 2: TopBar Status & Control Core)...");

    topbar::StatusManager status_mgr;

    // 1. Prepare AOT compiled binary UI
    std::string prism_file = "prism-topbar/ui/topbar.prism";
    std::string prismb_file = "prism-topbar/topbar.prismb";
    if (std::filesystem::exists(prism_file)) {
        std::ifstream in(prism_file);
        std::string dsl((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        compiler::Lexer lexer(dsl);
        auto tokens = lexer.Tokenize();
        compiler::Parser parser(tokens);
        auto ast = parser.Parse();
        if (ast) {
            compiler::BinaryGenerator gen;
            gen.WriteToFile(ast, prismb_file);
        }
    }

    // 2. Initialize Application via Prism SDK
    sdk::AppConfig config{};
    config.app_id = "prism_topbar";
    config.channel_name = channel;
    config.package_path = prismb_file;
    config.width = 1920;
    config.height = 34;

    auto app = sdk::Application::Create(config);
    if (!app) {
        PRISM_LOG_WARN("TOPBAR", "Running in offline standalone test mode (compositor channel not connected)");
    }

    if (app) {
        // Register reactive action observers
        app->On("launcher:toggle", [&](const ipc::EventPacket&) {
            PRISM_LOG_INFO("TOPBAR", "All Applications Drawer / Launcher toggled");
        });

        app->On("notifications:toggle", [&](const ipc::EventPacket&) {
            status_mgr.ToggleNotifications();
            app->SetState("notifications_btn", status_mgr.GetStatus().notification_str);
            PRISM_LOG_INFO("TOPBAR", "Notification center toggled (unread: %d)",
                           status_mgr.GetStatus().unread_notifications);
        });

        app->On("control:toggle", [&](const ipc::EventPacket&) {
            PRISM_LOG_INFO("TOPBAR", "Control Center quick settings toggled");
        });

        // Set initial state
        app->SetState("sys_badge", "PRISM");
        app->SetState("clock_time", status_mgr.GetStatus().time_str);
        app->SetState("net_status", status_mgr.GetStatus().network_str);
        app->SetState("bat_status", status_mgr.GetStatus().battery_str);

        app->Ready();
    }

    if (test_mode) {
        PRISM_LOG_INFO("TOPBAR", "Test mode: verifying status ticker and time formatting...");
        status_mgr.Update();
        assert(!status_mgr.GetStatus().time_str.empty());
        assert(!status_mgr.GetStatus().battery_str.empty());
        PRISM_LOG_INFO("TOPBAR", "Live TopBar time: '%s', Battery: '%s'",
                       status_mgr.GetStatus().time_str.c_str(), status_mgr.GetStatus().battery_str.c_str());
        PRISM_LOG_INFO("TOPBAR", "Prism-TopBar test completed successfully!");
        return 0;
    }

    // Background clock ticker thread
    std::atomic<bool> running{true};
    std::thread ticker([&]() {
        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            status_mgr.Update();
            if (app) {
                app->SetState("clock_time", status_mgr.GetStatus().time_str);
                app->SetState("bat_status", status_mgr.GetStatus().battery_str);
            }
        }
    });
    ticker.detach();

    int ret = app ? app->Exec() : 0;
    running.store(false);
    return ret;
}
