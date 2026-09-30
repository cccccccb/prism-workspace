#include "prism/core/logging.hpp"
#include "prism/tree/tree_engine.hpp"
#include "prism/wm/window.hpp"
#include <cassert>
#include <cmath>
#include <iostream>

using namespace prism;
using namespace prism::tree;

void TestSingleWindowSmartGaps()
{
    std::cout << "[TEST] 1. Single Window & Smart Gaps...\n";
    TreeEngine engine;
    auto spec = std::make_shared<TreeLayoutConfig>(TreeLayoutConfig{10, 12, true, 32});
    spec->inner_gap = 10;
    spec->outer_gap = 12;
    spec->smart_gaps = true;

    core::Rect screen{0, 0, 1920, 1080};
    auto win1 =
        std::make_shared<wm::Window>("term", "Terminal", core::Rect{0, 0, 100, 100}, nullptr);
    auto v1 = engine.InsertWindow(win1);
    assert(v1 != nullptr);
    assert(engine.GetFocusedWindow() == win1);

    engine.Arrange(screen, *spec);
    auto layout = engine.GetCalculatedLayout();
    assert(layout.size() == 1);
    assert(layout[0].first == win1);

    // Smart gaps: exactly full screen
    assert(layout[0].second.x == 0.0f);
    assert(layout[0].second.y == 0.0f);
    assert(layout[0].second.width == 1920.0f);
    assert(layout[0].second.height == 1080.0f);

    // Turn off smart gaps: outer gaps applied
    spec->smart_gaps = false;
    engine.Arrange(screen, *spec);
    layout = engine.GetCalculatedLayout();
    assert(layout[0].second.x == 12.0f);
    assert(layout[0].second.y == 12.0f);
    assert(layout[0].second.width == 1920.0f - 24.0f);
    assert(layout[0].second.height == 1080.0f - 24.0f);

    std::cout << "  -> Single Window & Smart Gaps PASSED\n";
}

void TestSplitHorizontal()
{
    std::cout << "[TEST] 2. Split Horizontal (2 & 3 Windows)...\n";
    TreeEngine engine;
    auto spec = std::make_shared<TreeLayoutConfig>(TreeLayoutConfig{10, 12, true, 32});
    spec->inner_gap = 10;
    spec->outer_gap = 12;
    spec->smart_gaps = true;

    core::Rect screen{0, 0, 1920, 1080};
    auto win1 = std::make_shared<wm::Window>("w1", "Window 1", core::Rect{}, nullptr);
    auto win2 = std::make_shared<wm::Window>("w2", "Window 2", core::Rect{}, nullptr);

    engine.InsertWindow(win1);
    engine.InsertWindow(win2, Direction::Right);

    engine.Arrange(screen, *spec);
    auto layout = engine.GetCalculatedLayout();
    assert(layout.size() == 2);

    float usable_w = 1920.0f - 24.0f - 10.0f;   // 1886
    float half_w = std::round(usable_w * 0.5f); // 943

    assert(layout[0].second.x == 12.0f);
    assert(layout[0].second.width == half_w);
    assert(layout[1].second.x == 12.0f + half_w + 10.0f);

    std::cout << "  -> Split Horizontal PASSED\n";
}

void TestRecursiveBspGrid()
{
    std::cout << "[TEST] 3. Recursive BSP Fission (2x2 Nested Grid)...\n";
    TreeEngine engine;
    auto spec = std::make_shared<TreeLayoutConfig>(TreeLayoutConfig{10, 12, true, 32});
    spec->inner_gap = 10;
    spec->outer_gap = 12;
    spec->smart_gaps = true;

    core::Rect screen{0, 0, 1920, 1080};
    auto win1 = std::make_shared<wm::Window>("w1", "Top-Left", core::Rect{}, nullptr);
    auto win2 = std::make_shared<wm::Window>("w2", "Top-Right", core::Rect{}, nullptr);
    auto win3 = std::make_shared<wm::Window>("w3", "Bottom-Left", core::Rect{}, nullptr);
    auto win4 = std::make_shared<wm::Window>("w4", "Bottom-Right", core::Rect{}, nullptr);

    // 1. win1 + win2 side-by-side
    auto v1 = engine.InsertWindow(win1);
    auto v2 = engine.InsertWindow(win2, Direction::Right, v1);

    // 2. Insert win3 below win1 -> Fissions left slot into SplitVertical container!
    auto v3 = engine.InsertWindow(win3, Direction::Down, v1);
    assert(v3 != nullptr);

    // 3. Insert win4 below win2 -> Fissions right slot into SplitVertical container!
    auto v4 = engine.InsertWindow(win4, Direction::Down, v2);
    assert(v4 != nullptr);

    engine.Arrange(screen, *spec);
    auto layout = engine.GetCalculatedLayout();
    assert(layout.size() == 4);

    // Find bounds for each window
    core::Rect r1{}, r2{}, r3{}, r4{};
    for (const auto &item : layout) {
        if (item.first == win1) {
            r1 = item.second;
        } else if (item.first == win2) {
            r2 = item.second;
        } else if (item.first == win3) {
            r3 = item.second;
        } else if (item.first == win4) {
            r4 = item.second;
        }
    }

    // Verify 2x2 grid geometry:
    // Left column: r1 and r3 share same X
    assert(r1.x == 12.0f);
    assert(r3.x == 12.0f);
    assert(r1.width == r3.width);
    assert(r3.y > r1.y); // r3 is below r1

    // Right column: r2 and r4 share same X
    assert(r2.x > r1.x + r1.width);
    assert(r4.x == r2.x);
    assert(r4.y > r2.y); // r4 is below r2

    std::cout << "  -> Recursive BSP Fission (2x2 Nested Grid) PASSED\n";
}

void TestGeometricFocusNavigation()
{
    std::cout << "[TEST] 4. Geometric Closest-Neighbour Focus Navigation...\n";
    TreeEngine engine;
    auto spec = std::make_shared<TreeLayoutConfig>(TreeLayoutConfig{10, 12, true, 32});
    spec->inner_gap = 10;
    spec->outer_gap = 12;

    core::Rect screen{0, 0, 1920, 1080};
    auto win1 = std::make_shared<wm::Window>("w1", "Top-Left", core::Rect{}, nullptr);
    auto win2 = std::make_shared<wm::Window>("w2", "Top-Right", core::Rect{}, nullptr);
    auto win3 = std::make_shared<wm::Window>("w3", "Bottom-Left", core::Rect{}, nullptr);
    auto win4 = std::make_shared<wm::Window>("w4", "Bottom-Right", core::Rect{}, nullptr);

    auto v1 = engine.InsertWindow(win1);
    auto v2 = engine.InsertWindow(win2, Direction::Right, v1);
    auto v3 = engine.InsertWindow(win3, Direction::Down, v1);
    auto v4 = engine.InsertWindow(win4, Direction::Down, v2);

    engine.Arrange(screen, *spec);

    // Start at Top-Left (win1)
    engine.SetFocusedWindow(win1);
    assert(engine.GetFocusedWindow() == win1);

    // 1. Move Right -> should land on Top-Right (win2)
    assert(engine.MoveFocus(Direction::Right));
    assert(engine.GetFocusedWindow() == win2);

    // 2. Move Down -> should land on Bottom-Right (win4)
    assert(engine.MoveFocus(Direction::Down));
    assert(engine.GetFocusedWindow() == win4);

    // 3. Move Left -> should land on Bottom-Left (win3)
    assert(engine.MoveFocus(Direction::Left));
    assert(engine.GetFocusedWindow() == win3);

    // 4. Move Up -> should land back on Top-Left (win1)
    assert(engine.MoveFocus(Direction::Up));
    assert(engine.GetFocusedWindow() == win1);

    std::cout << "  -> Geometric Closest-Neighbour Focus Navigation PASSED\n";
}

void TestTabbedAndStackedLayout()
{
    std::cout << "[TEST] 5. Tabbed & Stacked Container Grouping...\n";
    TreeEngine engine;
    auto spec = std::make_shared<TreeLayoutConfig>(TreeLayoutConfig{10, 12, true, 32});
    spec->header_height = 32.0f;

    core::Rect screen{0, 0, 1920, 1080};
    auto win1 = std::make_shared<wm::Window>("w1", "Tab 1", core::Rect{}, nullptr);
    auto win2 = std::make_shared<wm::Window>("w2", "Tab 2", core::Rect{}, nullptr);

    auto v1 = engine.InsertWindow(win1);
    auto tab_con = engine.GroupTabbed(v1, win2);
    assert(tab_con != nullptr);
    assert(tab_con->GetLayoutMode() == LayoutMode::Tabbed);
    assert(tab_con->GetChildren().size() == 2);

    engine.Arrange(screen, *spec);
    auto layout = engine.GetCalculatedLayout();
    assert(layout.size() == 2);

    // Both tabs share identical content area below the tab bar
    assert(layout[0].second.x == layout[1].second.x);
    assert(layout[0].second.width == layout[1].second.width);
    assert(layout[0].second.height == layout[1].second.height);
    assert(layout[0].second.y == 12.0f + 32.0f); // outer gap (12) + header height (32)

    std::cout << "  -> Tabbed & Stacked Container Grouping PASSED\n";
}

void TestWindowRemovalAndAutoPrune()
{
    std::cout << "[TEST] 6. Window Removal & Auto-Pruning...\n";
    TreeEngine engine;
    auto spec = std::make_shared<TreeLayoutConfig>(TreeLayoutConfig{10, 12, true, 32});
    core::Rect screen{0, 0, 1920, 1080};

    auto win1 = std::make_shared<wm::Window>("w1", "Window 1", core::Rect{}, nullptr);
    auto win2 = std::make_shared<wm::Window>("w2", "Window 2", core::Rect{}, nullptr);

    auto v1 = engine.InsertWindow(win1);
    auto v2 = engine.InsertWindow(win2, Direction::Right, v1);
    assert(engine.GetActiveWorkspace()->GetViewCount() == 2);

    // Remove win2
    assert(engine.RemoveWindow(win2));
    assert(engine.GetActiveWorkspace()->GetViewCount() == 1);
    assert(engine.GetFocusedWindow() == win1);

    // Arranging single window collapses gaps via smart gaps
    spec->smart_gaps = true;
    engine.Arrange(screen, *spec);
    auto layout = engine.GetCalculatedLayout();
    assert(layout.size() == 1);
    assert(layout[0].second.width == 1920.0f);

    std::cout << "  -> Window Removal & Auto-Pruning PASSED\n";
}

void TestWorkspaceSwitchingAndFocusMemory()
{
    std::cout << "[TEST] 7. Dynamic Workspaces & Focus Memory...\n";
    TreeEngine engine;
    auto win_ws1_a = std::make_shared<wm::Window>("w1a", "WS1 Window A", core::Rect{}, nullptr);
    auto win_ws1_b = std::make_shared<wm::Window>("w1b", "WS1 Window B", core::Rect{}, nullptr);
    auto win_ws2_a = std::make_shared<wm::Window>("w2a", "WS2 Window A", core::Rect{}, nullptr);

    // Workspace 1
    engine.InsertWindow(win_ws1_a);
    engine.InsertWindow(win_ws1_b, Direction::Right);
    engine.SetFocusedWindow(win_ws1_b);
    assert(engine.GetFocusedWindow() == win_ws1_b);

    // Switch to Workspace 2
    assert(engine.SwitchWorkspace("2"));
    assert(engine.GetActiveWorkspace()->GetName() == "2");
    engine.InsertWindow(win_ws2_a);
    assert(engine.GetFocusedWindow() == win_ws2_a);

    // Switch back to Workspace 1 -> verifies focus memory restores win_ws1_b!
    assert(engine.SwitchWorkspace("1"));
    assert(engine.GetActiveWorkspace()->GetName() == "1");
    assert(engine.GetFocusedWindow() == win_ws1_b);

    std::cout << "  -> Dynamic Workspaces & Focus Memory PASSED\n";
}

void TestSwapDirection()
{
    std::cout << "[TEST] 8. Geometric Window Swap Direction...\n";
    TreeEngine engine;
    auto spec = std::make_shared<TreeLayoutConfig>(TreeLayoutConfig{10, 12, true, 32});
    core::Rect screen{0, 0, 1920, 1080};

    auto win_a = std::make_shared<wm::Window>("wa", "Window A", core::Rect{}, nullptr);
    auto win_b = std::make_shared<wm::Window>("wb", "Window B", core::Rect{}, nullptr);

    engine.InsertWindow(win_a);
    engine.InsertWindow(win_b, Direction::Right);

    engine.Arrange(screen, *spec);
    auto layout1 = engine.GetCalculatedLayout();
    assert(layout1.size() == 2);
    assert(layout1[0].first == win_a);
    assert(layout1[1].first == win_b);

    // Focus on Window A (left), swap to the right
    engine.SetFocusedWindow(win_a);
    bool swapped = engine.SwapFocusDirection(Direction::Right);
    assert(swapped);

    engine.Arrange(screen, *spec);
    auto layout2 = engine.GetCalculatedLayout();
    assert(layout2.size() == 2);
    // After swap, Window B is on the left and Window A is on the right
    assert(layout2[0].first == win_b);
    assert(layout2[1].first == win_a);

    std::cout << "  -> Geometric Window Swap Direction PASSED\n";
}

void TestDumpTreeJson()
{
    std::cout << "[TEST] 9. Swaymsg-like JSON Tree Introspection...\n";
    TreeEngine engine;
    auto spec = std::make_shared<TreeLayoutConfig>(TreeLayoutConfig{10, 12, true, 32});
    core::Rect screen{0, 0, 1920, 1080};

    auto win1 = std::make_shared<wm::Window>("editor", "Prism Code", core::Rect{}, nullptr);
    auto win2 = std::make_shared<wm::Window>("term", "Terminal", core::Rect{}, nullptr);

    engine.InsertWindow(win1);
    engine.InsertWindow(win2, Direction::Right);
    engine.Arrange(screen, *spec);

    std::string json = engine.DumpTreeJson();
    assert(!json.empty());
    assert(json.find("\"type\":\"root\"") != std::string::npos);
    assert(json.find("\"active_workspace\":\"1\"") != std::string::npos);
    assert(json.find("\"type\":\"workspace\"") != std::string::npos);
    assert(json.find("\"type\":\"container\"") != std::string::npos);
    assert(json.find("\"type\":\"view\"") != std::string::npos);
    assert(json.find("Prism Code") != std::string::npos);
    assert(json.find("Terminal") != std::string::npos);

    std::cout << "  -> JSON Tree Output: " << json.substr(0, 120) << "...\n";
    std::cout << "  -> Swaymsg-like JSON Tree Introspection PASSED\n";
}

void TestDragToSplitSimulation()
{
    std::cout << "[TEST] 10. Titlebar Drag-to-Split Fission Lifecycle...\n";
    TreeEngine engine;
    auto spec = std::make_shared<TreeLayoutConfig>(TreeLayoutConfig{10, 12, true, 32});
    spec->smart_gaps = false;
    spec->inner_gap = 10;
    spec->outer_gap = 10;
    core::Rect screen{0, 0, 1920, 1080};

    auto win1 = std::make_shared<wm::Window>("w1", "Editor", core::Rect{}, nullptr);
    auto win2 = std::make_shared<wm::Window>("w2", "Browser", core::Rect{}, nullptr);
    auto win3 = std::make_shared<wm::Window>("w3", "Terminal", core::Rect{}, nullptr);

    engine.InsertWindow(win1);
    engine.InsertWindow(win2, Direction::Right);
    engine.InsertWindow(win3, Direction::Right);

    // Initial 3-split
    engine.Arrange(screen, *spec);
    assert(engine.GetCalculatedLayout().size() == 3);

    // Simulate drag: User grabs win3 and drops at the bottom of win2 (BottomSplit)
    engine.RemoveWindow(win3);
    auto target_view = engine.FindViewForWindow(win2);
    assert(target_view != nullptr);
    engine.InsertWindow(win3, Direction::Down, target_view);

    engine.Arrange(screen, *spec);
    auto layout = engine.GetCalculatedLayout();
    assert(layout.size() == 3);

    // Verify layout: win1 is on left, win2 is top-right, win3 is bottom-right!
    auto r1 = engine.FindViewForWindow(win1)->GetBounds();
    auto r2 = engine.FindViewForWindow(win2)->GetBounds();
    auto r3 = engine.FindViewForWindow(win3)->GetBounds();

    assert(r1.x < r2.x);
    assert(r2.x == r3.x);
    assert(r2.y < r3.y);

    std::cout << "  -> Titlebar Drag-to-Split Fission Lifecycle PASSED\n";
}

void TestSmallAreasAndLargeGaps()
{
    TreeEngine engine;
    std::vector<std::shared_ptr<wm::Window>> windows;
    for (int i = 0; i < 4; ++i) {
        windows.push_back(std::make_shared<wm::Window>("small", "Small", core::Rect{}, nullptr));
    }
    auto first = engine.InsertWindow(windows[0]);
    auto second = engine.InsertWindow(windows[1], Direction::Right, first);
    engine.InsertWindow(windows[2], Direction::Down, first);
    engine.InsertWindow(windows[3], Direction::Down, second);
    const TreeLayoutConfig oversized{256, 256, false, 0};
    for (const core::Rect screen : {core::Rect{5, 7, 100, 40}, core::Rect{5, 7, 24, 9},
                                    core::Rect{5, 7, 1, 1}, core::Rect{5, 7, .5f, .5f}}) {
        engine.Arrange(screen, oversized);
        const auto layout = engine.GetCalculatedLayout();
        assert(layout.size() == 4);
        for (const auto &[window, rect] : layout) {
            assert(rect.width > 0 && rect.height > 0);
            assert(rect.x >= screen.x && rect.y >= screen.y);
            assert(rect.x + rect.width <= screen.x + screen.width + .0001f);
            assert(rect.y + rect.height <= screen.y + screen.height + .0001f);
        }
        auto check = [&](auto &&self, const std::shared_ptr<TreeNode> &node) -> void {
            if (auto container = std::dynamic_pointer_cast<ContainerNode>(node);
                container && !container->GetChildren().empty()) {
                const bool horizontal = container->GetLayoutMode() == LayoutMode::SplitHorizontal;
                const auto &last = container->GetChildren().back()->GetBounds();
                assert(std::abs((horizontal ? last.x + last.width : last.y + last.height) -
                                (horizontal ? node->GetBounds().x + node->GetBounds().width
                                            : node->GetBounds().y + node->GetBounds().height)) <
                       .0001f);
                for (std::size_t i = 1; i < container->GetChildren().size(); ++i) {
                    const auto &a = container->GetChildren()[i - 1]->GetBounds();
                    const auto &b = container->GetChildren()[i]->GetBounds();
                    assert(horizontal ? a.x + a.width <= b.x + .0001f
                                      : a.y + a.height <= b.y + .0001f);
                }
            }
            for (const auto &child : node->GetChildren()) {
                self(self, child);
            }
        };
        check(check, engine.GetActiveWorkspace());
    }
    // Ordinary gap sizes still preserve user split fractions and exact coverage.
    TreeEngine ratio;
    auto left = ratio.InsertWindow(windows[0]);
    auto right = ratio.InsertWindow(windows[1], Direction::Right, left);
    left->SetFractions(.25, left->GetHeightFraction());
    right->SetFractions(.75, right->GetHeightFraction());
    ratio.Arrange({0, 0, 110, 40}, {10, 0, false, 0});
    assert(left->GetBounds().width == 25 && right->GetBounds().width == 75);
    std::cout << "  -> Small areas, large gaps, positive nested geometry and split ratios PASSED\n";
}

int main()
{
    std::cout << "========================================================\n";
    std::cout << "  PrismWM Multi-Level Recursive BSP Tree Engine Tests  \n";
    std::cout << "========================================================\n";

    TestSingleWindowSmartGaps();
    TestSplitHorizontal();
    TestRecursiveBspGrid();
    TestGeometricFocusNavigation();
    TestTabbedAndStackedLayout();
    TestWindowRemovalAndAutoPrune();
    TestWorkspaceSwitchingAndFocusMemory();
    TestSwapDirection();
    TestDumpTreeJson();
    TestDragToSplitSimulation();
    TestSmallAreasAndLargeGaps();
    // Removing a background workspace's final client must retain its root.
    // A later activation must not restore a removed node or stale focus flag.
    {
        TreeEngine engine;
        auto a = std::make_shared<wm::Window>("a", "A", core::Rect{}, nullptr);
        auto b = std::make_shared<wm::Window>("b", "B", core::Rect{}, nullptr);
        engine.InsertWindow(a);
        auto first = engine.GetActiveWorkspace();
        engine.SwitchWorkspace("2");
        engine.InsertWindow(b);
        assert(engine.RemoveWindow(a));
        assert(first->GetRootContainer()->GetParent() == first);
        assert(engine.GetFocusedWindow() == b && b->IsFocused());
        engine.SwitchWorkspace("1");
        assert(!engine.GetFocusedWindow() && !b->IsFocused());
        engine.InsertWindow(a);
        assert(first->GetViewCount() == 1);
        assert(engine.MoveWindowToWorkspace(a, "2"));
        assert(first->GetViewCount() == 0 && !engine.GetFocusedWindow());
        engine.SetFocusedWindow(a);
        assert(engine.GetActiveWorkspace()->GetName() == "2" && engine.GetFocusedWindow() == a);
    }

    std::cout << "\n>>> ALL 10 MULTI-LEVEL RECURSIVE BSP TREE TESTS PASSED CLEANLY! <<<\n";
    return 0;
}
