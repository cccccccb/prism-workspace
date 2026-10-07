#pragma once

#include "prism/contracts/contour.hpp"
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
    constexpr bool operator==(const Color &) const noexcept = default;
};

struct FillRect {
    LogicalRect bounds{};
    Color color{};
    bool operator==(const FillRect &) const noexcept = default;
};

struct FillRoundedRect {
    LogicalRect bounds{};
    double radius{0.0};
    Color color{};
    bool operator==(const FillRoundedRect &) const noexcept = default;
};

struct StrokeRoundedRect {
    LogicalRect bounds{};
    double radius{0};
    double width{1};
    Color color{};
    bool operator==(const StrokeRoundedRect &) const noexcept = default;
};

struct RoundedRectShadow {
    LogicalRect bounds{};
    double radius{0};
    double blur{0}; // Gaussian sigma in logical pixels
    double offset_y{0};
    Color color{};
    bool inset{false};
    bool operator==(const RoundedRectShadow &) const noexcept = default;
};
// SDK vector resources; these are geometric icons, independent of installed fonts.
enum class VectorIcon {
    Grid,
    Music,
    Settings,
    Folder,
    Terminal,
    Play,
    Pause,
    Previous,
    Next,
    Volume,
    Wifi,
    Battery,
    Search,
    Sun,
    Moon,
    Power,
    Check,
    Chevron,
    Refresh,
    Cpu,
    Memory,
    Heart,
    Layers,
    Rectangle,
    Drop,
    WifiOff,
    Error,
    Fullscreen,
    Restore,
    SplitHorizontal,
    SplitVertical,
    Document,
    DocumentAdd,
    Save,
    Close,
    ArrowLeft,
    ArrowRight,
    Trash,
    Info,
    Monitor,
    Brush,
    Activity,
    Clock,
    Repeat,
    HeartOutline
};

struct DrawIcon {
    VectorIcon icon{VectorIcon::Grid};
    LogicalRect bounds{};
    Color color{};
    bool operator==(const DrawIcon &) const noexcept = default;
};
enum class ImageFit { Fill, Contain, Cover };

struct DrawImage {
    ResourceId image{};
    LogicalRect destination{};
    ImageFit fit{ImageFit::Fill};
    bool operator==(const DrawImage &) const noexcept = default;
};

struct GlyphPlacement {
    std::uint32_t glyph_index{0};
    LogicalPoint origin{};
    bool operator==(const GlyphPlacement &) const noexcept = default;
};

struct DrawGlyphRun {
    ResourceId font{};
    std::vector<GlyphPlacement> glyphs;
    Color color{};
    double font_size{16.0};
    bool operator==(const DrawGlyphRun &) const noexcept = default;
};

struct PushClipRect {
    LogicalRect bounds{};
    bool operator==(const PushClipRect &) const noexcept = default;
};

struct PushClipRoundedRect {
    LogicalRect bounds{};
    double radius{0};
    bool operator==(const PushClipRoundedRect &) const noexcept = default;
};

struct PopClip {
    bool operator==(const PopClip &) const noexcept = default;
};

// Row-major 2D affine transform: [a, c, tx, b, d, ty].
struct PushTransform {
    std::array<double, 6> values{1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
    bool operator==(const PushTransform &) const noexcept = default;
};

struct PopTransform {
    bool operator==(const PopTransform &) const noexcept = default;
};

// Composite the complete enclosed subtree once at this opacity. Overlapping
// children retain their own source-over result before the group is blended.
struct PushOpacity {
    double opacity{1.0};
    bool operator==(const PushOpacity &) const noexcept = default;
};

struct PopOpacity {
    bool operator==(const PopOpacity &) const noexcept = default;
};

struct FillContour {
    Contour contour;
    Color color{};
    bool operator==(const FillContour &) const noexcept = default;
};

// Width is measured inward from the boundary. Zero draws no border.
struct StrokeContour {
    Contour contour;
    double width{1};
    Color color{};
    bool operator==(const StrokeContour &) const noexcept = default;
};

struct ContourShadow {
    Contour contour;
    double blur{0}; // Gaussian sigma in logical pixels
    double offset_y{0};
    Color color{};
    bool inset{false};
    bool operator==(const ContourShadow &) const noexcept = default;
};

struct PushClipContour {
    Contour contour;
    bool operator==(const PushClipContour &) const noexcept = default;
};

using DrawCommand =
    std::variant<FillRect, FillRoundedRect, StrokeRoundedRect, RoundedRectShadow, DrawIcon,
                 DrawImage, DrawGlyphRun, PushClipRect, PushClipRoundedRect, PopClip, PushTransform,
                 PopTransform, PushOpacity, PopOpacity, FillContour, StrokeContour, ContourShadow,
                 PushClipContour>;

// In-process, ordered renderer input. Logical coordinates map to the target canvas;
// caller owns any output scale. Clips/transforms/opacity use matched, nested pairs.
// Font/image ResourceIds must be registered with the renderer before replay and stay
// registered through replay. Never serialize this C++ object across IPC.
// Full replay clears the target transparent; partial replay clears only the
// declared repair region and preserves pixels outside it. CPU BGRA8888 and
// GLES RGBA8888 output contain premultiplied alpha. Colors/images are straight alpha.
struct DisplayList {
    WindowId window{};
    std::uint64_t generation{0};
    std::vector<DrawCommand> commands;
};

} // namespace prism::contracts
