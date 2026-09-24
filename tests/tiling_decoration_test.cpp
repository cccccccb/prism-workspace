#include "prism/decoration/tiling_decoration_spec.hpp"
#include "prism/decoration/tiling_window_decorator.hpp"
#include "prism/decoration/tiling_drag_manager.hpp"
#include "prism/compiler/binary_loader.hpp"
#include "prism/compiler/lexer.hpp"
#include "prism/compiler/parser.hpp"
#include "prism/compiler/binary_generator.hpp"
#include "prism/wm/window.hpp"
#include "prism/core/logging.hpp"
#include <cassert>
#include <iostream>
#include <vector>

using namespace prism;
using namespace prism::decoration;

void TestDecorationSpec() {
    std::cout << "[TEST] 1. TilingDecorationSpec Presets...\n";
    auto def = TilingDecorationSpec::CreateDefault();
    assert(def != nullptr);
    assert(def->theme_name == "DefaultTilingGlass");
    assert(def->gaps.inner == 10);
    assert(def->border.width == 2.0f);
    assert(def->header.show_header == true);

    auto nord = TilingDecorationSpec::CreateNordicGlass();
    assert(nord != nullptr);
    assert(nord->theme_name == "NordicGlass");
    assert(nord->gaps.inner == 14);

    auto i3 = TilingDecorationSpec::CreateMinimalI3();
    assert(i3 != nullptr);
    assert(i3->theme_name == "MinimalI3");
    assert(i3->border.corner_radius == 0.0f);
    assert(i3->header.height == 24.0f);
    std::cout << "  -> TilingDecorationSpec Presets PASSED\n";
}

void TestWindowDecoratorGeometryAndHitTest() {
    std::cout << "[TEST] 2. TilingWindowDecorator Geometry & Hit-Testing...\n";
    core::Rect initial_bounds{100.0f, 100.0f, 800.0f, 600.0f};
    auto win = std::make_shared<wm::Window>("test_app", "Test Window", initial_bounds, nullptr);

    auto spec = TilingDecorationSpec::CreateDefault();
    spec->border.width = 2.0f;
    spec->header.height = 32.0f;

    auto decorator = std::make_unique<TilingWindowDecorator>(win.get(), spec);
    decorator->ApplyGeometry(initial_bounds);

    // Verify sub-rect bounds
    auto header_bounds = decorator->GetHeaderBounds();
    assert(header_bounds.x == 102.0f);
    assert(header_bounds.y == 102.0f);
    assert(header_bounds.width == 800.0f - 4.0f);
    assert(header_bounds.height == 32.0f);

    auto content_bounds = decorator->GetContentBounds();
    assert(content_bounds.x == 102.0f);
    assert(content_bounds.y == 102.0f + 32.0f);
    assert(content_bounds.width == 800.0f - 4.0f);
    assert(content_bounds.height == 600.0f - 4.0f - 32.0f);

    // Verify Hit-Testing:
    // Inside Close button (approx x=18, y=14 relative to window)
    assert(decorator->HitTestHeader(18.0f, 14.0f) == HeaderAction::Close);

    // Inside Split toggle button (approx x=38, y=14)
    assert(decorator->HitTestHeader(38.0f, 14.0f) == HeaderAction::ToggleSplit);

    // Inside Fold button (approx x=58, y=14)
    assert(decorator->HitTestHeader(58.0f, 14.0f) == HeaderAction::ToggleFold);

    // Inside Monocle button (approx x=78, y=14)
    assert(decorator->HitTestHeader(78.0f, 14.0f) == HeaderAction::ToggleMonocle);

    // On header blank region (approx x=200, y=14) -> initiates titlebar drag!
    assert(decorator->HitTestHeader(200.0f, 14.0f) == HeaderAction::TitlebarDrag);

    // Below header inside window content (x=200, y=100) -> None
    assert(decorator->HitTestHeader(200.0f, 100.0f) == HeaderAction::None);

    // Outside window bounds (x=-10, y=14) -> None
    assert(decorator->HitTestHeader(-10.0f, 14.0f) == HeaderAction::None);

    // Focus state
    assert(!decorator->IsFocused());
    decorator->SetFocused(true);
    assert(decorator->IsFocused());

    std::cout << "  -> TilingWindowDecorator Geometry & Hit-Testing PASSED\n";
}

void TestTilingDragManager() {
    std::cout << "[TEST] 3. TilingDragManager Drag-to-Split & Quadrants...\n";
    core::Rect b1{0.0f, 30.0f, 960.0f, 1050.0f};
    core::Rect b2{960.0f, 30.0f, 960.0f, 1050.0f};

    auto win1 = std::make_shared<wm::Window>("app1", "Tile 1", b1, nullptr);
    auto win2 = std::make_shared<wm::Window>("app2", "Tile 2", b2, nullptr);
    std::vector<std::shared_ptr<wm::Window>> windows = {win1, win2};

    TilingDragManager drag_mgr;
    assert(!drag_mgr.IsDragging());

    // 1. Begin drag on win1
    assert(drag_mgr.BeginDrag(win1, 100.0f, 45.0f));
    assert(drag_mgr.IsDragging());
    assert(drag_mgr.GetDraggedWindow() == win1);

    // 2. Drag over win2's Left 10% (x=960 + 96 = 1056)
    drag_mgr.UpdateDrag(1056.0f, 500.0f, windows);
    assert(drag_mgr.GetCurrentQuadrant() == DropQuadrant::LeftSplit);
    auto preview = drag_mgr.GetPreviewBounds();
    assert(preview.x == 960.0f);
    assert(preview.width == 480.0f);

    // 3. Drag over win2's Right 90% (x=960 + 864 = 1824)
    drag_mgr.UpdateDrag(1824.0f, 500.0f, windows);
    assert(drag_mgr.GetCurrentQuadrant() == DropQuadrant::RightSplit);
    preview = drag_mgr.GetPreviewBounds();
    assert(preview.x == 960.0f + 480.0f);
    assert(preview.width == 480.0f);

    // 4. Drag over win2's Center (x=960 + 480 = 1440, y=550) -> Swap!
    drag_mgr.UpdateDrag(1440.0f, 550.0f, windows);
    assert(drag_mgr.GetCurrentQuadrant() == DropQuadrant::Swap);
    preview = drag_mgr.GetPreviewBounds();
    assert(preview.x == 960.0f && preview.width == 960.0f);

    // 5. Commit drag
    auto res = drag_mgr.EndDrag();
    assert(res.executed == true);
    assert(res.source_window == win1);
    assert(res.target_window == win2);
    assert(res.quadrant == DropQuadrant::Swap);
    assert(!drag_mgr.IsDragging());

    std::cout << "  -> TilingDragManager Drag-to-Split & Quadrants PASSED\n";
}

void TestThemeAotCompilationAndLoading() {
    std::cout << "[TEST] 4. AOT Theme DSL Compilation & Zero-Copy Loading...\n";
    std::string dsl_source = R"(
        TilingDecoration("CustomAmberGlass") {
            gaps(inner: 18, outer: 22, smart: false)
            border(width: 3.0, focused: #FFB300, unfocused: #424242, specular: #FFE082, cornerRadius: 16)
            backdrop(focused: #261E14, unfocused: #1A140E, blur: 35, passes: 5)
            header(height: 36, show: true, focused: #3E2723, unfocused: #211510)
            dropZone(fill: #FFB30040, border: #FFB300, width: 3)
            motion {
                fold(engine: spring, damping: 0.85, stiffness: 240, duration: 250)
                fullscreen(engine: bezier, duration: 300, bezier: [0.16, 1.0, 0.3, 1.0])
                splitMove(engine: spring, damping: 0.78, stiffness: 280, duration: 220)
                focus(engine: bezier, duration: 180, bezier: [0.25, 0.1, 0.25, 1.0])
            }
        }
    )";

    // 1. Compile DSL in-memory
    compiler::Lexer lexer(dsl_source);
    auto tokens = lexer.Tokenize();
    compiler::Parser parser(tokens);
    auto ast = parser.Parse();
    assert(ast != nullptr);
    assert(ast->type == compiler::BinaryNodeType::TilingDecoration);

    // 2. Generate AOT binary
    compiler::BinaryGenerator gen;
    std::string test_bin_path = "/tmp/test_amber.prismb";
    assert(gen.WriteToFile(ast, test_bin_path));

    // 3. Load via BinaryThemeLoader
    auto theme = compiler::BinaryThemeLoader::LoadFromFile(test_bin_path);
    assert(theme != nullptr);
    assert(theme->theme_name == "CustomAmberGlass");
    assert(theme->gaps.inner == 18);
    assert(theme->gaps.outer == 22);
    assert(theme->gaps.smart_gaps == false);
    assert(theme->border.width == 3.0f);
    assert(theme->border.corner_radius == 16.0f);
    assert(theme->backdrop.blur_radius == 35.0f);
    assert(theme->backdrop.blur_passes == 5);
    assert(theme->header.height == 36.0f);
    assert(theme->drop_zone.border_width == 3.0f);

    // Verify parsed kinetic motion properties
    assert(theme->motion.fold.engine == MotionEngine::Spring);
    assert(theme->motion.fold.damping >= 0.84f && theme->motion.fold.damping <= 0.86f);
    assert(theme->motion.fold.stiffness == 240.0f);
    assert(theme->motion.fullscreen.engine == MotionEngine::CubicBezier);
    assert(theme->motion.fullscreen.duration_ms == 300.0f);
    assert(std::abs(theme->motion.fullscreen.bezier_x1 - 0.16f) < 0.01f);
    assert(theme->motion.split_move.stiffness == 280.0f);

    // 4. Apply to window decorator
    core::Rect b{0, 0, 800, 600};
    auto win = std::make_shared<wm::Window>("app", "Test Window", b, nullptr);
    auto decorator = std::make_unique<TilingWindowDecorator>(win.get(), theme);
    decorator->ApplyGeometry(b);
    assert(decorator->GetHeaderBounds().height == 36.0f);
    assert(decorator->GetHeaderBounds().x == 3.0f); // border width

    std::cout << "  -> AOT Theme DSL Compilation & Zero-Copy Loading PASSED\n";
}

void TestKineticMotionAndAnimation() {
    std::cout << "[TEST] 5. Kinetic Motion Physics, Springs & Decorator Transitions...\n";

    // 1. SpringSolver convergence test
    SpringSolver spring;
    spring.SnapTo(0.0f);
    spring.SetTarget(100.0f, 0.80f, 240.0f);
    assert(!spring.IsSettled());

    // Advance 60 frames @ 60 FPS (1 second)
    for (int i = 0; i < 60; ++i) {
        spring.Step(0.016f);
    }
    assert(spring.IsSettled());
    assert(std::abs(spring.pos - 100.0f) < 0.01f);
    assert(spring.vel == 0.0f);

    // 2. CubicBezierEvaluator curve test
    float val_start = CubicBezierEvaluator::Solve(0.16f, 1.0f, 0.3f, 1.0f, 0.0f);
    float val_mid = CubicBezierEvaluator::Solve(0.16f, 1.0f, 0.3f, 1.0f, 0.5f);
    float val_end = CubicBezierEvaluator::Solve(0.16f, 1.0f, 0.3f, 1.0f, 1.0f);
    assert(val_start == 0.0f);
    assert(val_mid > 0.8f); // Fast initial acceleration curve
    assert(val_end == 1.0f);

    // 3. Decorator AnimateToBounds (Split movement transition)
    core::Rect b_start{0.0f, 30.0f, 400.0f, 600.0f};
    core::Rect b_target{0.0f, 30.0f, 800.0f, 600.0f};
    auto win = std::make_shared<wm::Window>("anim_app", "Animated Tile", b_start, nullptr);
    auto spec = TilingDecorationSpec::CreateNordicGlass();
    auto dec = std::make_unique<TilingWindowDecorator>(win.get(), spec);
    dec->ApplyGeometry(b_start);

    dec->AnimateToBounds(b_target, MotionType::SplitMove);
    assert(dec->IsAnimating());

    // Step animation for up to 60 frames (~1.0s)
    for (int i = 0; i < 60 && dec->IsAnimating(); ++i) {
        dec->StepAnimation(0.016f);
    }
    assert(!dec->IsAnimating());
    auto final_vis = dec->GetVisualBounds();
    assert(std::abs(final_vis.width - 800.0f) < 0.1f);

    // 4. Decorator Fold / Roll-up to Titlebar transition
    assert(!dec->IsFolded());
    dec->ToggleFold();
    assert(dec->IsFolded());
    assert(dec->IsAnimating());

    // Step fold animation to completion
    for (int i = 0; i < 40; ++i) {
        dec->StepAnimation(0.016f);
    }
    assert(!dec->IsAnimating());
    auto folded_bounds = dec->GetVisualBounds();
    float expected_folded_h = spec->header.height + 2.0f * spec->border.width;
    assert(std::abs(folded_bounds.height - expected_folded_h) < 0.2f);

    // Unfold back
    dec->ToggleFold();
    assert(!dec->IsFolded());
    for (int i = 0; i < 40; ++i) {
        dec->StepAnimation(0.016f);
    }
    assert(!dec->IsAnimating());
    auto unfolded_bounds = dec->GetVisualBounds();
    assert(std::abs(unfolded_bounds.height - 600.0f) < 0.2f);

    std::cout << "  -> Kinetic Motion Physics, Springs & Decorator Transitions PASSED\n";
}

int main() {
    std::cout << "=================================================\n";
    std::cout << "  PrismWM Tiling Window Decoration System Tests  \n";
    std::cout << "=================================================\n";

    TestDecorationSpec();
    TestWindowDecoratorGeometryAndHitTest();
    TestTilingDragManager();
    TestThemeAotCompilationAndLoading();
    TestKineticMotionAndAnimation();

    std::cout << "\n>>> ALL TILING DECORATION & KINETIC MOTION TESTS PASSED CLEANLY! <<<\n";
    return 0;
}
