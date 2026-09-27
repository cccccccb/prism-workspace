#pragma once
#include <cstdint>
#include <vector>

namespace prism::contracts {
// Integer buffer pixels, top-left origin. Runtime clips to the current target;
// the platform alone converts to EGL's bottom-left coordinates.
struct DamageRect {
    std::int32_t x{}, y{}, width{}, height{};
    constexpr bool operator==(const DamageRect&) const noexcept = default;
};
// full is authoritative and uses no rectangles. Empty non-full means unchanged
// content, never unknown buffer contents. Content damage and buffer repair use
// this representation but have different histories and submission roles.
struct DamageRegion {
    bool full{false};
    std::vector<DamageRect> rects;
    static DamageRegion Full() { return {true, {}}; }
    bool operator==(const DamageRegion&) const noexcept = default;
};
} // namespace prism::contracts
