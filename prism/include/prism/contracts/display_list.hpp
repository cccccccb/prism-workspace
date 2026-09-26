#pragma once

#include "prism/contracts/types.hpp"
#include <array>
#include <cstdint>
#include <variant>
#include <vector>

namespace prism::contracts {

// Straight-alpha sRGB input; source-over compositing. The renderer chooses its internal format.
struct Color {
    std::uint8_t r{0};
    std::uint8_t g{0};
    std::uint8_t b{0};
    std::uint8_t a{255};
    constexpr bool operator==(const Color&) const noexcept = default;
};

struct FillRect {
    LogicalRect bounds{};
    Color color{};
};

struct FillRoundedRect {
    LogicalRect bounds{};
    double radius{0.0};
    Color color{};
};

struct StrokeRoundedRect { LogicalRect bounds{}; double radius{0}; double width{1}; Color color{}; };
struct RoundedRectShadow {
    LogicalRect bounds{};
    double radius{0};
    double blur{0}; // Gaussian sigma in logical pixels
    double offset_y{0};
    Color color{};
    bool inset{false};
};
// SDK vector resources; these are geometric icons, independent of installed fonts.
enum class VectorIcon { Grid, Music, Settings, Folder, Terminal, Play, Pause, Previous,
    Next, Volume, Wifi, Battery, Search, Sun, Moon, Power, Check, Chevron, Refresh, Cpu, Memory, Heart };
struct DrawIcon { VectorIcon icon{VectorIcon::Grid}; LogicalRect bounds{}; Color color{}; };
enum class ImageFit { Fill, Contain, Cover };

struct DrawImage {
    ResourceId image{};
    LogicalRect destination{};
    ImageFit fit{ImageFit::Fill};
};

struct GlyphPlacement {
    std::uint32_t glyph_index{0};
    LogicalPoint origin{};
};

struct DrawGlyphRun {
    ResourceId font{};
    std::vector<GlyphPlacement> glyphs;
    Color color{};
    double font_size{16.0};
};

struct PushClipRect { LogicalRect bounds{}; };
struct PushClipRoundedRect { LogicalRect bounds{}; double radius{0}; };
struct PopClip {};

// Row-major 2D affine transform: [a, c, tx, b, d, ty].
struct PushTransform {
    std::array<double, 6> values{1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
};
struct PopTransform {};

using DrawCommand = std::variant<FillRect, FillRoundedRect, StrokeRoundedRect,
    RoundedRectShadow, DrawIcon, DrawImage, DrawGlyphRun, PushClipRect,
    PushClipRoundedRect, PopClip, PushTransform, PopTransform>;

// In-process, ordered renderer input. Logical coordinates map to the target canvas;
// caller owns any output scale. Clips/transforms use matched, nested push/pop pairs.
// Font/image ResourceIds must be registered with the renderer before replay and stay
// registered through replay. Never serialize this C++ object across IPC.
// Every target is cleared transparent; CPU BGRA8888 and GLES RGBA8888 output
// contain premultiplied alpha. Colors and decoded images remain straight alpha.
struct DisplayList {
    WindowId window{};
    std::uint64_t generation{0};
    std::vector<DrawCommand> commands;
};

} // namespace prism::contracts
