#include "wallpaper_manager.hpp"
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
#include <cassert>

using namespace prism;

int main(int argc, char* argv[]) {
    std::string channel = (argc > 1) ? argv[1] : "/prism_desktop_ipc";
    std::string wp_dir = (argc > 2) ? argv[2] : "resources/wallpapers";

    bool test_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--test" || std::string(argv[i]) == "--auto-exit") {
            test_mode = true;
        }
    }

    PRISM_LOG_INFO("DESKTOP", "Starting Prism-Desktop Shell (Layer 1: Desktop)...");

    desktop::WallpaperManager wp_mgr(wp_dir);

    // 1. Prepare AOT compiled binary UI if needed
    std::string prism_file = "prism-desktop/ui/desktop.prism";
    if (!std::filesystem::exists(prism_file)) {
        if (std::filesystem::exists("../prism-desktop/ui/desktop.prism")) {
            prism_file = "../prism-desktop/ui/desktop.prism";
        } else if (std::filesystem::exists("/usr/share/prism/ui/desktop.prism")) {
            prism_file = "/usr/share/prism/ui/desktop.prism";
        }
    }
    std::string runtime_dir = getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "/tmp";
    std::string prismb_file = std::filesystem::exists("prism-desktop/ui/desktop.prism")
        ? "prism-desktop/desktop.prismb"
        : (runtime_dir + "/desktop.prismb");

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
    config.app_id = "prism_desktop";
    config.channel_name = channel;
    config.package_path = prismb_file;
    config.width = 1920;
    config.height = 1080;

    auto app = sdk::Application::Create(config);
    if (!app) {
        PRISM_LOG_WARN("DESKTOP", "Running in offline standalone test mode (compositor channel not connected)");
    }

    // Initial wallpaper render to client surface
    if (app && app->GetSurface()) {
        wp_mgr.RenderTo(*app->GetSurface());
    }

    if (app) {
        // Register reactive action observers
        app->On("desktop:next_wallpaper", [&](const ipc::EventPacket&) {
            wp_mgr.NextWallpaper();
            if (const auto* cur = wp_mgr.GetCurrentWallpaper()) {
                app->SetState("wallpaper_title", cur->name);
                if (app->GetSurface()) wp_mgr.RenderTo(*app->GetSurface());
            }
        });

        app->On("desktop:prev_wallpaper", [&](const ipc::EventPacket&) {
            wp_mgr.PrevWallpaper();
            if (const auto* cur = wp_mgr.GetCurrentWallpaper()) {
                app->SetState("wallpaper_title", cur->name);
                if (app->GetSurface()) wp_mgr.RenderTo(*app->GetSurface());
            }
        });

        app->On("desktop:theme_sunset", [&](const ipc::EventPacket&) {
            wp_mgr.SetWallpaperById("sunset_anime");
            if (const auto* cur = wp_mgr.GetCurrentWallpaper()) {
                app->SetState("wallpaper_title", cur->name);
                if (app->GetSurface()) wp_mgr.RenderTo(*app->GetSurface());
            }
        });

        // Set initial state
        if (const auto* cur = wp_mgr.GetCurrentWallpaper()) {
            app->SetState("wallpaper_title", cur->name);
            app->SetState("desktop_badge", "PRISM-DESKTOP");
        }

        app->Ready();
    }

    if (test_mode) {
        PRISM_LOG_INFO("DESKTOP", "Test mode: verifying wallpaper switching and surface render...");
        wp_mgr.NextWallpaper();
        assert(wp_mgr.GetCurrentWallpaper() != nullptr);
        render::FrameBuffer test_fb(1920, 1080);
        wp_mgr.RenderTo(test_fb);
        assert(test_fb.GetPixels()[540 * 1920 + 960] != 0);
        PRISM_LOG_INFO("DESKTOP", "Prism-Desktop test completed successfully!");
        return 0;
    }

    return app ? app->Exec() : 0;
}
