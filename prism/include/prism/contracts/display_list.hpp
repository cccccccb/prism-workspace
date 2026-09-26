#pragma once

#include "prism/contracts/types.hpp"
#include <array>
#include <cstdint>
#include <variant>
#include <vector>

namespace prism::contracts {

// Straight-alpha sRGB input; the renderer chooses its internal color format.
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

struct DrawImage {
    ResourceId image{};
    LogicalRect destination{};
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
struct PopClip {};

// Row-major 2D affine transform: [a, c, tx, b, d, ty].
struct PushTransform {
    std::array<double, 6> values{1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
};
struct PopTransform {};

using DrawCommand = std::variant<FillRect, FillRoundedRect, DrawImage,
    DrawGlyphRun, PushClipRect, PopClip, PushTransform, PopTransform>;

// In-process, ordered renderer input. Never serialize this C++ object across IPC.
struct DisplayList {
    WindowId window{};
    std::uint64_t generation{0};
    std::vector<DrawCommand> commands;
};

} // namespace prism::contracts
