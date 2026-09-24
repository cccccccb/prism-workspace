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
    return spec;
}

} // namespace prism::decoration
