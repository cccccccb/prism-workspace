#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

namespace prism::core {

struct Rect {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};

    constexpr bool operator==(const Rect &) const noexcept = default;
};

struct Color {
    std::uint8_t r{0};
    std::uint8_t g{0};
    std::uint8_t b{0};
    std::uint8_t a{255};

    // DSL and .prismb colors use 0xRRGGBBAA (see Lexer::ScanHexColor).
    static constexpr Color FromHex(std::uint32_t rgba) noexcept
    {
        return Color{static_cast<std::uint8_t>(rgba >> 24), static_cast<std::uint8_t>(rgba >> 16),
                     static_cast<std::uint8_t>(rgba >> 8), static_cast<std::uint8_t>(rgba)};
    }

    // FrameBuffer drawing functions use 0xAARRGGBB.
    constexpr std::uint32_t ToHex() const noexcept
    {
        return (static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(r) << 16) |
               (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
    }
};

using Timestamp = std::uint64_t;

inline Timestamp CurrentTimeNs() noexcept
{
    return static_cast<Timestamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count());
}

inline Timestamp CurrentTimeUs() noexcept
{
    return CurrentTimeNs() / 1000;
}

// The compiler stores "$name", while SDK callers normally pass "name".
constexpr std::uint32_t HashSlot(std::string_view name) noexcept
{
    if (!name.empty() && name.front() == '$') {
        name.remove_prefix(1);
    }
    std::uint32_t hash = 2166136261u;
    for (char ch : name) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 16777619u;
    }
    return hash;
}

enum class GestureType { Swipe3FingerUp, PinchZoom };

} // namespace prism::core
