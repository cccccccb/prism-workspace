#include "prism/wm/compositor.hpp"
#include "prism/wm/window.hpp"
#include "prism/wm/layer_type.hpp"
#include "prism/compiler/lexer.hpp"
#include "prism/compiler/parser.hpp"
#include "prism/compiler/binary_generator.hpp"
#include "prism/compiler/binary_loader.hpp"
#include "prism/render/framebuffer.hpp"
#include "prism/render/canvas_renderer.hpp"
#include "prism/core/logging.hpp"
#include <cassert>
#include <iostream>
#include <fstream>
#include <memory>
#include <filesystem>

using namespace prism;

std::shared_ptr<scene::SceneNode> CompileDsl(const std::string& dsl_path, const std::string& bin_path) {
    if (!std::filesystem::exists(dsl_path)) {
        return nullptr;
    }
    std::ifstream in(dsl_path);
    std::string dsl((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    compiler::Lexer lexer(dsl);
    auto tokens = lexer.Tokenize();
    compiler::Parser parser(tokens);
    auto ast = parser.Parse();
    if (!ast) return nullptr;

    compiler::BinaryGenerator gen;
    gen.WriteToFile(ast, bin_path);
    return compiler::BinarySceneLoader::LoadFromFile(bin_path);
}

void TestExperimentalShellIntegration() {
    std::cout << "[TEST] Experimental Shell Integration (Desktop + TopBar + Dock + App)...\n";

    wm::Compositor comp;
    assert(comp.Initialize());

    // 1. Desktop Layer (Wallpaper Canvas)
    auto desk_tree = CompileDsl("prism-desktop/ui/desktop.prism", "/tmp/exp_desktop.prismb");
    auto desk_win = comp.CreateWindow("prism_desktop", "Desktop", core::Rect{0, 0, 1920, 1080}, "/prism_exp_desk_ipc", wm::LayerType::Desktop);
    assert(desk_win != nullptr);
    if (desk_tree) {
        desk_win->SetPreviewTree(desk_tree);
        desk_win->SetMasterTree(desk_tree);
    }

    // Attach high-res wallpaper surface to Desktop window
    auto wallpaper_fb = std::make_shared<render::FrameBuffer>(1920, 1080);
    std::string wp_file = "resources/wallpapers/sunset_anime.png";
    if (std::filesystem::exists(wp_file)) {
        wallpaper_fb->LoadImage(wp_file);
    } else {
        wallpaper_fb->DrawDesktopGradient();
    }
    desk_win->AttachSurface(wallpaper_fb);

    // 2. TopBar Layer (Mac-style status & control center matching 1.png)
    auto topbar_tree = CompileDsl("prism-topbar/ui/topbar.prism", "/tmp/exp_topbar.prismb");
    auto topbar_win = comp.CreateWindow("prism_topbar", "TopBar", core::Rect{0, 0, 1920, 34}, "/prism_exp_tb_ipc", wm::LayerType::TopBar);
    assert(topbar_win != nullptr);
    assert(topbar_win->GetExclusiveMargin() == 34.0f);
    if (topbar_tree) {
        topbar_win->SetPreviewTree(topbar_tree);
        topbar_win->SetMasterTree(topbar_tree);
    }
    // Update live slots to match reference 1.png
    topbar_win->UpdateSlot(core::HashSlot("clock_time"), "Oct-11 15:13:24");
    topbar_win->UpdateSlot(core::HashSlot("sys_badge"), "PRISM");
    topbar_win->UpdateSlot(core::HashSlot("net_status"), "Wi-Fi 5G");
    topbar_win->UpdateSlot(core::HashSlot("bat_status"), "100% ⚡");

    // 3. Dock Layer (Mac-style floating dock matching 1.png)
    auto dock_tree = CompileDsl("prism-dock/ui/dock.prism", "/tmp/exp_dock.prismb");
    auto dock_win = comp.CreateWindow("prism_dock", "Dock", core::Rect{560, 998, 800, 72}, "/prism_exp_dk_ipc", wm::LayerType::Dock);
    assert(dock_win != nullptr);
    assert(dock_win->GetExclusiveMargin() == 82.0f); // 72 + 10 margin
    if (dock_tree) {
        dock_win->SetPreviewTree(dock_tree);
        dock_win->SetMasterTree(dock_tree);
    }
    dock_win->UpdateSlot(core::HashSlot("running_badge"), "● 5 Active");

    // 4. Regular Application in Workspace (AppGroup Layer)
    auto app_win = comp.CreateWindow("demo_player", "Prism Music Studio", core::Rect{200, 100, 800, 600}, "/prism_exp_app_ipc", wm::LayerType::App);
    assert(app_win != nullptr);

    // Step physics & arrange layout inside safe negotiated area
    comp.Tick(0.016f);

    auto app_bounds = app_win->GetBounds();
    std::cout << "  -> App bounds negotiated: y=" << app_bounds.y << ", h=" << app_bounds.height << "\n";
    assert(app_bounds.y >= 34.0f);
    assert(app_bounds.y + app_bounds.height <= (1080.0f - 82.0f));

    // 5. Full Composite Framebuffer Render
    render::FrameBuffer fb(1920, 1080);
    comp.RenderToFrameBuffer(fb);

    // Verify non-empty pixels in all 3 layer regions
    // Desktop wallpaper region
    assert(fb.GetPixels()[500 * 1920 + 200] != 0);
    // TopBar region
    assert(fb.GetPixels()[16 * 1920 + 960] != 0);
    // Dock region
    assert(fb.GetPixels()[1030 * 1920 + 960] != 0);

    // Export experimental snapshot
    std::filesystem::create_directories("snapshots");
    std::string snap_ppm = "snapshots/shell_experimental_preview.ppm";
    std::string snap_png = "snapshots/shell_experimental_preview.png";
    fb.SavePPM(snap_ppm);

    std::string cmd = "python3 -c \"from PIL import Image; Image.open('" + snap_ppm + "').save('" + snap_png + "')\" 2>/dev/null";
    std::system(cmd.c_str());

    PRISM_LOG_INFO("TEST", "Saved full experimental composite snapshot to %s", snap_png.c_str());

    // Cleanup tmp files
    std::filesystem::remove("/tmp/exp_desktop.prismb");
    std::filesystem::remove("/tmp/exp_topbar.prismb");
    std::filesystem::remove("/tmp/exp_dock.prismb");

    std::cout << "  -> Experimental Shell Integration Test PASSED!\n";
}

int main() {
    std::cout << "========================================================\n";
    std::cout << "  PRISM SHELL EXPERIMENTAL TEST (DESKTOP/TOPBAR/DOCK)   \n";
    std::cout << "========================================================\n";

    TestExperimentalShellIntegration();

    std::cout << "========================================================\n";
    std::cout << "  ALL EXPERIMENTAL SHELL TESTS PASSED!                  \n";
    std::cout << "========================================================\n";
    return 0;
}
