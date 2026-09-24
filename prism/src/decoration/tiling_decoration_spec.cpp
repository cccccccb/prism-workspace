#include "prism/decoration/tiling_decoration_spec.hpp"

namespace prism::decoration {

std::shared_ptr<TilingDecorationSpec> TilingDecorationSpec::CreateDefault() {
    auto spec = std::make_shared<TilingDecorationSpec>();
    spec->theme_name = "DefaultTilingGlass";
    spec->gaps.inner = 10;
    spec->gaps.outer = 12;
    spec->gaps.smart_gaps = true;
    spec->border.width = 2.0f;
    spec->border.color_focused = core::Color{0, 122, 255, 230};
    spec->border.color_unfocused = core::Color{255, 255, 255, 30};
    spec->border.top_rim_specular = core::Color{255, 255, 255, 50};
    spec->border.corner_radius = 12.0f;

    // Kinetic Motion defaults
    spec->motion.fold.engine = MotionEngine::Spring;
    spec->motion.fold.damping = 0.82f;
    spec->motion.fold.stiffness = 240.0f;
    spec->motion.fold.duration_ms = 250.0f;
    spec->motion.fold.clip_content = true;
    spec->motion.fold.fade_content = true;

    spec->motion.fullscreen.engine = MotionEngine::CubicBezier;
    spec->motion.fullscreen.duration_ms = 280.0f;
    spec->motion.fullscreen.bezier_x1 = 0.16f;
    spec->motion.fullscreen.bezier_y1 = 1.00f;
    spec->motion.fullscreen.bezier_x2 = 0.30f;
    spec->motion.fullscreen.bezier_y2 = 1.00f;
    spec->motion.fullscreen.smart_gaps_collapse = true;

    spec->motion.split_move.engine = MotionEngine::Spring;
    spec->motion.split_move.damping = 0.76f;
    spec->motion.split_move.stiffness = 260.0f;
    spec->motion.split_move.duration_ms = 200.0f;

    spec->motion.focus.engine = MotionEngine::CubicBezier;
    spec->motion.focus.duration_ms = 150.0f;
    spec->motion.focus.bezier_x1 = 0.25f;
    spec->motion.focus.bezier_y1 = 0.10f;
    spec->motion.focus.bezier_x2 = 0.25f;
    spec->motion.focus.bezier_y2 = 1.00f;

    return spec;
}

std::shared_ptr<TilingDecorationSpec> TilingDecorationSpec::CreateNordicGlass() {
    auto spec = std::make_shared<TilingDecorationSpec>();
    spec->theme_name = "NordicGlass";
    spec->gaps.inner = 14;
    spec->gaps.outer = 16;
    spec->gaps.smart_gaps = true;
    spec->border.width = 1.5f;
    spec->border.color_focused = core::Color{136, 192, 208, 240}; // Frost Cyan #88C0D0
    spec->border.color_unfocused = core::Color{76, 86, 106, 120}; // Polar Night muted #4C566A
    spec->border.top_rim_specular = core::Color{236, 239, 244, 40};
    spec->backdrop.bg_focused = core::Color{46, 52, 64, 235};     // Nord Dark #2E3440
    spec->backdrop.bg_unfocused = core::Color{36, 41, 51, 220};
    spec->header.bg_focused = core::Color{59, 66, 82, 240};
    spec->header.bg_unfocused = core::Color{46, 52, 64, 220};
    spec->header.title_focused = core::Color{236, 239, 244, 255};
    spec->header.title_unfocused = core::Color{140, 150, 168, 180};

    // Nordic Glass motion curves
    spec->motion.fold.engine = MotionEngine::Spring;
    spec->motion.fold.damping = 0.85f;
    spec->motion.fold.stiffness = 240.0f;
    spec->motion.fold.duration_ms = 260.0f;

    spec->motion.fullscreen.engine = MotionEngine::CubicBezier;
    spec->motion.fullscreen.duration_ms = 300.0f;
    spec->motion.fullscreen.bezier_x1 = 0.16f;
    spec->motion.fullscreen.bezier_y1 = 1.00f;
    spec->motion.fullscreen.bezier_x2 = 0.30f;
    spec->motion.fullscreen.bezier_y2 = 1.00f;

    spec->motion.split_move.engine = MotionEngine::Spring;
    spec->motion.split_move.damping = 0.78f;
    spec->motion.split_move.stiffness = 280.0f;
    spec->motion.split_move.duration_ms = 220.0f;

    spec->motion.focus.engine = MotionEngine::CubicBezier;
    spec->motion.focus.duration_ms = 180.0f;
    return spec;
}

std::shared_ptr<TilingDecorationSpec> TilingDecorationSpec::CreateMinimalI3() {
    auto spec = std::make_shared<TilingDecorationSpec>();
    spec->theme_name = "MinimalI3";
    spec->gaps.inner = 4;
    spec->gaps.outer = 4;
    spec->gaps.smart_gaps = true;
    spec->border.width = 2.0f;
    spec->border.color_focused = core::Color{40, 85, 142, 255};   // i3 classic focused blue
    spec->border.color_unfocused = core::Color{51, 51, 51, 255};  // i3 inactive dark gray
    spec->border.top_rim_specular = core::Color{255, 255, 255, 20};
    spec->border.corner_radius = 0.0f; // Sharp borders
    spec->header.height = 24.0f;
    spec->header.bg_focused = core::Color{40, 85, 142, 255};
    spec->header.bg_unfocused = core::Color{34, 34, 34, 255};

    // Minimal i3 snappy curves
    spec->motion.fold.engine = MotionEngine::Spring;
    spec->motion.fold.damping = 0.95f;
    spec->motion.fold.stiffness = 450.0f;
    spec->motion.fold.duration_ms = 100.0f;

    spec->motion.fullscreen.engine = MotionEngine::CubicBezier;
    spec->motion.fullscreen.duration_ms = 120.0f;

    spec->motion.split_move.engine = MotionEngine::Spring;
    spec->motion.split_move.damping = 0.95f;
    spec->motion.split_move.stiffness = 450.0f;
    spec->motion.split_move.duration_ms = 100.0f;

    spec->motion.focus.engine = MotionEngine::CubicBezier;
    spec->motion.focus.duration_ms = 80.0f;
    return spec;
}

} // namespace prism::decoration
