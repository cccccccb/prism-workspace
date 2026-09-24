#pragma once

#include "prism/core/types.hpp"
#include <string>
#include <memory>

namespace prism::decoration {

struct GapsSpec {
    int inner{10};       // Gap between adjacent tiles (px)
    int outer{12};       // Gap between outer tiles and screen edges (px)
    bool smart_gaps{true}; // If true, single window takes full screen without gaps
};

struct BorderSpec {
    float width{2.0f};
    core::Color color_focused{0, 122, 255, 230};     // Vibrant Apple Blue: #007AFF
    core::Color color_unfocused{255, 255, 255, 30};  // Muted translucent white: #FFFFFF1E
    core::Color top_rim_specular{255, 255, 255, 50}; // Top 1px specular highlight
    float corner_radius{12.0f};
};

struct BackdropSpec {
    core::Color bg_focused{20, 24, 34, 230};         // #141822 ~90% opacity
    core::Color bg_unfocused{14, 16, 22, 217};       // #0E1016 ~85% opacity
    float blur_radius{28.0f};
    int blur_passes{4};
};

struct HeaderSpec {
    float height{32.0f};
    bool show_header{true};
    core::Color bg_focused{28, 33, 46, 240};
    core::Color bg_unfocused{18, 20, 28, 220};
    core::Color title_focused{255, 255, 255, 255};
    core::Color title_unfocused{160, 165, 175, 180};
    bool show_tiling_controls{true};
};

struct DropZoneSpec {
    core::Color fill_color{0, 122, 255, 60};        // Semi-transparent blue fill
    core::Color border_color{0, 122, 255, 230};     // Sharp blue boundary
    float border_width{2.0f};
};

class TilingDecorationSpec {
public:
    std::string theme_name{"DefaultTilingGlass"};
    GapsSpec gaps;
    BorderSpec border;
    BackdropSpec backdrop;
    HeaderSpec header;
    DropZoneSpec drop_zone;

    static std::shared_ptr<TilingDecorationSpec> CreateDefault();
    static std::shared_ptr<TilingDecorationSpec> CreateNordicGlass();
    static std::shared_ptr<TilingDecorationSpec> CreateMinimalI3();
};

} // namespace prism::decoration
