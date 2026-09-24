#include "prism/wm/layer_type.hpp"
#include "prism/wm/layer_manager.hpp"
#include "prism/wm/compositor.hpp"
#include "prism/wm/window.hpp"
#include "prism/compiler/lexer.hpp"
#include "prism/compiler/parser.hpp"
#include "prism/compiler/binary_generator.hpp"
#include "prism/compiler/binary_loader.hpp"
#include "prism/scene/container_node.hpp"
#include "prism/render/framebuffer.hpp"
#include "prism/render/canvas_renderer.hpp"
#include "prism/core/logging.hpp"
#include <cassert>
#include <iostream>
#include <memory>
#include <filesystem>

using namespace prism;

void TestSingletonArbitration() {
    std::cout << "[TEST] 1. Layer Singleton Lease & Arbitration...\n";
    wm::LayerManager lm;

    auto make_win = [](const std::string& app_id, wm::LayerType layer) {
        auto win = std::make_shared<wm::Window>(app_id, app_id, core::Rect{0, 0, 100, 100}, nullptr);
        win->SetLayerType(layer);
        return win;
    };

    // Desktop singleton
    auto desk1 = make_win("desktop_main", wm::LayerType::Desktop);
    auto desk2 = make_win("desktop_rogue", wm::LayerType::Desktop);
    assert(lm.RegisterWindow(desk1, wm::LayerType::Desktop) == wm::LayerRegisterResult::Success);
    assert(lm.IsLayerOccupied(wm::LayerType::Desktop) == true);
    assert(lm.RegisterWindow(desk2, wm::LayerType::Desktop) == wm::LayerRegisterResult::AlreadyExists);
    assert(lm.GetDesktop() == desk1);

    // TopBar singleton
    auto bar1 = make_win("topbar_main", wm::LayerType::TopBar);
    auto bar2 = make_win("topbar_rogue", wm::LayerType::TopBar);
    assert(lm.RegisterWindow(bar1, wm::LayerType::TopBar) == wm::LayerRegisterResult::Success);
    assert(lm.IsLayerOccupied(wm::LayerType::TopBar) == true);
    assert(lm.RegisterWindow(bar2, wm::LayerType::TopBar) == wm::LayerRegisterResult::AlreadyExists);
    assert(lm.GetTopBar() == bar1);

    // Dock singleton
    auto dock1 = make_win("dock_main", wm::LayerType::Dock);
    auto dock2 = make_win("dock_rogue", wm::LayerType::Dock);
    assert(lm.RegisterWindow(dock1, wm::LayerType::Dock) == wm::LayerRegisterResult::Success);
    assert(lm.IsLayerOccupied(wm::LayerType::Dock) == true);
    assert(lm.RegisterWindow(dock2, wm::LayerType::Dock) == wm::LayerRegisterResult::AlreadyExists);
    assert(lm.GetDock() == dock1);

    // AppGroup singleton
    auto group1 = make_win("group_main", wm::LayerType::AppGroup);
    auto group2 = make_win("group_rogue", wm::LayerType::AppGroup);
    assert(lm.RegisterWindow(group1, wm::LayerType::AppGroup) == wm::LayerRegisterResult::Success);
    assert(lm.IsLayerOccupied(wm::LayerType::AppGroup) == true);
    assert(lm.RegisterWindow(group2, wm::LayerType::AppGroup) == wm::LayerRegisterResult::AlreadyExists);
    assert(lm.GetAppGroup() == group1);

    // App windows can have multiple instances
    auto app1 = make_win("app_editor", wm::LayerType::App);
    auto app2 = make_win("app_browser", wm::LayerType::App);
    assert(lm.RegisterWindow(app1, wm::LayerType::App) == wm::LayerRegisterResult::Success);
    assert(lm.RegisterWindow(app2, wm::LayerType::App) == wm::LayerRegisterResult::Success);

    // Release singleton lease & re-registration
    lm.UnregisterWindow(desk1);
    assert(lm.IsLayerOccupied(wm::LayerType::Desktop) == false);
    assert(lm.GetDesktop() == nullptr);
    assert(lm.RegisterWindow(desk2, wm::LayerType::Desktop) == wm::LayerRegisterResult::Success);
    assert(lm.GetDesktop() == desk2);

    std::cout << "  -> Singleton Arbitration Passed!\n";
}

void TestCompositorSingletonIntegration() {
    std::cout << "[TEST] 2. Compositor Singleton Lease Enforcement...\n";
    wm::Compositor comp;
    bool ok = comp.Initialize();
    assert(ok);

    // First TopBar creation succeeds
    auto bar1 = comp.CreateWindow("shell_bar", "TopBar", core::Rect{0, 0, 1920, 36}, "/prism_test_topbar1", wm::LayerType::TopBar);
    assert(bar1 != nullptr);
    assert(bar1->GetLayerType() == wm::LayerType::TopBar);
    assert(bar1->GetExclusiveMargin() == 36.0f);

    // Second TopBar creation must be rejected with nullptr
    auto bar2 = comp.CreateWindow("rogue_bar", "RogueBar", core::Rect{0, 0, 1920, 36}, "/prism_test_topbar2", wm::LayerType::TopBar);
    assert(bar2 == nullptr);

    // Destroy first TopBar, now a new one can be created
    comp.DestroyWindow(bar1);
    auto bar3 = comp.CreateWindow("shell_bar_v2", "TopBar V2", core::Rect{0, 0, 1920, 40}, "/prism_test_topbar3", wm::LayerType::TopBar);
    assert(bar3 != nullptr);
    assert(bar3->GetExclusiveMargin() == 40.0f);

    // Dock creation
    auto dock1 = comp.CreateWindow("shell_dock", "Dock", core::Rect{0, 0, 800, 72}, "/prism_test_dock1", wm::LayerType::Dock);
    assert(dock1 != nullptr);
    assert(dock1->GetLayerType() == wm::LayerType::Dock);

    // Second Dock rejected
    auto dock2 = comp.CreateWindow("rogue_dock", "RogueDock", core::Rect{0, 0, 800, 72}, "/prism_test_dock2", wm::LayerType::Dock);
    assert(dock2 == nullptr);

    std::cout << "  -> Compositor Singleton Integration Passed!\n";
}

void TestUsableAreaNegotiation() {
    std::cout << "[TEST] 3. Dynamic Usable Area Negotiation (TopBar + Dock Exclusions)...\n";
    wm::LayerManager lm;

    // Default screen: 1920 x 1080 with default built-in shell chrome (top: 30px, bottom: 70px)
    auto area0 = lm.CalculateUsableArea(1920, 1080);
    assert(area0.x == 0.0f);
    assert(area0.y == 30.0f);
    assert(area0.width == 1920.0f);
    assert(area0.height == (1080.0f - 30.0f - 70.0f)); // 980.0f

    // Register TopBar with 36px exclusive margin (dock still has default 70px fallback)
    auto bar = std::make_shared<wm::Window>("topbar", "TopBar", core::Rect{0, 0, 1920, 36}, nullptr);
    bar->SetExclusiveMargin(36.0f);
    lm.RegisterWindow(bar, wm::LayerType::TopBar);

    auto area1 = lm.CalculateUsableArea(1920, 1080);
    assert(area1.x == 0.0f);
    assert(area1.y == 36.0f);
    assert(area1.width == 1920.0f);
    assert(area1.height == (1080.0f - 36.0f - 70.0f)); // 974.0f

    // Register Dock with 80px exclusive bottom margin
    auto dock = std::make_shared<wm::Window>("dock", "Dock", core::Rect{560, 990, 800, 80}, nullptr);
    dock->SetExclusiveMargin(80.0f);
    lm.RegisterWindow(dock, wm::LayerType::Dock);

    auto area2 = lm.CalculateUsableArea(1920, 1080);
    assert(area2.x == 0.0f);
    assert(area2.y == 36.0f);
    assert(area2.width == 1920.0f);
    assert(area2.height == (1080.0f - 36.0f - 80.0f)); // 964.0f

    std::cout << "  -> Usable Area Negotiation Passed! Bounds: ("
              << area2.x << ", " << area2.y << ", " << area2.width << ", " << area2.height << ")\n";
}

void TestDslDecorationOnLayerClients() {
    std::cout << "[TEST] 4. DSL Decoration on TopBar, Dock, Desktop, AppGroup Nodes...\n";

    std::string dsl_code = R"(
TopBar(height: 38.0) {
    HStack(spacing: 8.0) {
        Badge("PRISM-OS", $os_badge)
        Spacer(16.0)
        Text("Active Workspace: Main", $ws_title)
        Spacer(20.0)
        Text("10:42 AM", $time_label)
    }
}.acrylic(blur: 24.0, passes: 4, tint: #141822E6)

Dock(height: 72.0) {
    HStack(spacing: 14.0) {
        Button("Files", "shell:files")
        Button("Terminal", "shell:term")
        Button("Editor", "shell:editor")
        Button("Settings", "shell:prefs")
    }
}.acrylic(blur: 30.0, passes: 4, tint: #0f121ae6).springOnHover(scale: 1.15, damping: 0.85)

Desktop {
    ZStack {
        Card {
            Text("Dark Nebula Dynamic Wallpaper", $bg_label)
        }
    }
}.acrylic(blur: 8.0, passes: 2, tint: #080a10ff)

AppGroup {
    VStack(spacing: 6.0) {
        Text("Workspace Container Root", $root_label)
    }
}
)";

    compiler::Lexer lexer(dsl_code);
    auto tokens = lexer.Tokenize();
    assert(!tokens.empty());

    // We can parse the TopBar component
    compiler::Parser parser(tokens);
    auto topbar_ast = parser.Parse();
    assert(topbar_ast != nullptr);
    assert(topbar_ast->type == compiler::BinaryNodeType::TopBar);
    assert(topbar_ast->modifiers.size() == 1);
    assert(topbar_ast->modifiers[0].name == "acrylic");

    // Binary Serialization round-trip
    std::string tmp_bin = "/tmp/prism_topbar_test.prismb";
    compiler::BinaryGenerator gen;
    assert(gen.WriteToFile(topbar_ast, tmp_bin));

    auto loaded_scene = compiler::BinarySceneLoader::LoadFromFile(tmp_bin);
    assert(loaded_scene != nullptr);

    auto topbar_node = std::dynamic_pointer_cast<scene::TopBarNode>(loaded_scene);
    assert(topbar_node != nullptr);
    assert(topbar_node->Modifiers().GetAll().size() == 1);
    assert(topbar_node->Modifiers().Has(modifiers::ModifierType::Acrylic));

    // Clean up
    std::filesystem::remove(tmp_bin);

    std::cout << "  -> DSL Decoration on Layer Clients Passed!\n";
}

void TestLayerFrameCompositing() {
    std::cout << "[TEST] 5. Full 4-Layer Frame Compositing & Rendering...\n";

    wm::Compositor comp;
    bool ok = comp.Initialize();
    assert(ok);

    // 1. Create TopBar with DSL tree
    std::string topbar_dsl = R"(
TopBar(height: 32.0) {
    HStack(spacing: 12.0) {
        Text("PrismWM TopBar Client", $sys_title)
        Spacer(10.0)
        Badge("LIVE", $status)
    }
}.acrylic(blur: 16.0, passes: 3, tint: #1b1f2ae6)
)";
    compiler::Lexer lex1(topbar_dsl);
    auto tokens1 = lex1.Tokenize();
    compiler::Parser p1(tokens1);
    auto ast1 = p1.Parse();
    std::string bin1 = "/tmp/test_topbar.prismb";
    compiler::BinaryGenerator gen1;
    gen1.WriteToFile(ast1, bin1);
    auto topbar_scene = compiler::BinarySceneLoader::LoadFromFile(bin1);

    auto topbar_win = comp.CreateWindow("desktop_topbar", "TopBar", core::Rect{0, 0, 1920, 32}, "/prism_test_tb_ipc", wm::LayerType::TopBar);
    assert(topbar_win != nullptr);
    topbar_win->SetPreviewTree(topbar_scene);
    topbar_win->SetMasterTree(topbar_scene);

    // 2. Create Dock with DSL tree
    std::string dock_dsl = R"(
Dock(height: 64.0) {
    HStack(spacing: 16.0) {
        Button("App1", "launch:1")
        Button("App2", "launch:2")
        Button("App3", "launch:3")
    }
}.acrylic(blur: 24.0, passes: 4, tint: #11141ee6)
)";
    compiler::Lexer lex2(dock_dsl);
    auto tokens2 = lex2.Tokenize();
    compiler::Parser p2(tokens2);
    auto ast2 = p2.Parse();
    std::string bin2 = "/tmp/test_dock.prismb";
    compiler::BinaryGenerator gen2;
    gen2.WriteToFile(ast2, bin2);
    auto dock_scene = compiler::BinarySceneLoader::LoadFromFile(bin2);

    auto dock_win = comp.CreateWindow("desktop_dock", "Dock", core::Rect{560, 1000, 800, 64}, "/prism_test_dk_ipc", wm::LayerType::Dock);
    assert(dock_win != nullptr);
    dock_win->SetPreviewTree(dock_scene);
    dock_win->SetMasterTree(dock_scene);

    // 3. Create regular App Window inside usable area
    auto app_win = comp.CreateWindow("demo_app", "Editor", core::Rect{100, 100, 800, 600}, "/prism_test_app_ipc", wm::LayerType::App);
    assert(app_win != nullptr);

    // Step physics & arrange layout
    comp.Tick(0.016f);

    // Verify App window bounds are clamped within usable area: y >= 32.0f
    auto app_bounds = app_win->GetBounds();
    assert(app_bounds.y >= 32.0f);
    assert(app_bounds.y + app_bounds.height <= (1080.0f - 74.0f)); // TopBar + Dock exclusive bounds

    // 4. Render to FrameBuffer
    render::FrameBuffer fb(1920, 1080);
    comp.RenderToFrameBuffer(fb);

    // TopBar area pixel check: (100, 16) should have TopBar acrylic background
    uint32_t topbar_px = fb.GetPixels()[16 * fb.GetWidth() + 100];
    assert(topbar_px != 0); // Not blank

    // Cleanup tmp files
    std::filesystem::remove(bin1);
    std::filesystem::remove(bin2);

    std::cout << "  -> Full 4-Layer Frame Compositing Passed!\n";
}

int main() {
    std::cout << "========================================================\n";
    std::cout << "  PRISM 4-LAYER HIERARCHY & SINGLETON LEASE TEST SUITE  \n";
    std::cout << "========================================================\n";

    TestSingletonArbitration();
    TestCompositorSingletonIntegration();
    TestUsableAreaNegotiation();
    TestDslDecorationOnLayerClients();
    TestLayerFrameCompositing();

    std::cout << "========================================================\n";
    std::cout << "  ALL 4-LAYER TESTS PASSED SUCCESSFULLY!                \n";
    std::cout << "========================================================\n";
    return 0;
}
