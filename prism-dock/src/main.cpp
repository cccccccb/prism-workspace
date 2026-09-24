#include "dock_item.hpp"
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
#include <cassert>

using namespace prism;

int main(int argc, char* argv[]) {
    std::string channel = (argc > 1) ? argv[1] : "/prism_dock_ipc";

    bool test_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--test" || std::string(argv[i]) == "--auto-exit") {
            test_mode = true;
        }
    }

    PRISM_LOG_INFO("DOCK", "Starting Prism-Dock Shell (Layer 3: Mac-style Floating Application Dock)...");

    dock::DockManager dock_mgr;

    // 1. Prepare AOT compiled binary UI
    std::string prism_file = "prism-dock/ui/dock.prism";
    if (!std::filesystem::exists(prism_file)) {
        if (std::filesystem::exists("../prism-dock/ui/dock.prism")) {
            prism_file = "../prism-dock/ui/dock.prism";
        } else if (std::filesystem::exists("/usr/share/prism/ui/dock.prism")) {
            prism_file = "/usr/share/prism/ui/dock.prism";
        }
    }
    std::string runtime_dir = getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "/tmp";
    std::string prismb_file = std::filesystem::exists("prism-dock/ui/dock.prism")
        ? "prism-dock/dock.prismb"
        : (runtime_dir + "/dock.prismb");

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
    config.app_id = "prism_dock";
    config.channel_name = channel;
    config.package_path = prismb_file;
    config.width = 800;
    config.height = 72;

    auto app = sdk::Application::Create(config);
    if (!app) {
        PRISM_LOG_WARN("DOCK", "Running in offline standalone test mode (compositor channel not connected)");
    }

    auto update_running_state = [&]() {
        if (app) {
            size_t running = dock_mgr.GetRunningCount();
            app->SetState("running_badge", "● " + std::to_string(running) + " Active");
        }
    };

    if (app) {
        // Register reactive action observers
        app->On("dock:launcher", [&](const ipc::EventPacket&) {
            PRISM_LOG_INFO("DOCK", "Launcher grid button clicked");
        });

        auto bind_launch = [&](const std::string& action, const std::string& app_id) {
            app->On(action, [&dock_mgr, update_running_state, app_id](const ipc::EventPacket&) {
                dock_mgr.LaunchOrActivate(app_id);
                update_running_state();
            });
        };

        bind_launch("app:launch:files", "files");
        bind_launch("app:launch:terminal", "terminal");
        bind_launch("app:launch:browser", "browser");
        bind_launch("app:launch:editor", "editor");
        bind_launch("app:launch:music", "music");
        bind_launch("app:launch:settings", "settings");

        update_running_state();
        app->Ready();
    }

    if (test_mode) {
        PRISM_LOG_INFO("DOCK", "Test mode: verifying app launch and running count...");
        size_t initial_count = dock_mgr.GetRunningCount();
        dock_mgr.LaunchOrActivate("editor");
        assert(dock_mgr.GetRunningCount() == initial_count + 1);
        PRISM_LOG_INFO("DOCK", "Prism-Dock test completed successfully!");
        return 0;
    }

    return app ? app->Exec() : 0;
}
