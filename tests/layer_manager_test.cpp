#include "prism/wm/layer_type.hpp"
#include "prism/wm/layer_manager.hpp"
#include "prism/wm/compositor.hpp"
#include "prism/wm/window.hpp"
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

int main() {
    std::cout << "========================================================\n";
    std::cout << "  PRISM 4-LAYER HIERARCHY & SINGLETON LEASE TEST SUITE  \n";
    std::cout << "========================================================\n";

    TestSingletonArbitration();
    TestCompositorSingletonIntegration();
    TestUsableAreaNegotiation();

    std::cout << "========================================================\n";
    std::cout << "  ALL 4-LAYER TESTS PASSED SUCCESSFULLY!                \n";
    std::cout << "========================================================\n";
    return 0;
}
