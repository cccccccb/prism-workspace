#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace prism::contracts {
enum class MotionEasing : std::uint8_t { Linear, EaseInCubic, EaseOutCubic, EaseInOutCubic };

struct MotionTransition {
    std::string name;
    std::uint32_t duration_ms{};
    MotionEasing easing{MotionEasing::Linear};
    bool operator==(const MotionTransition &) const = default;
};

struct MotionSet {
    std::string id;
    std::vector<MotionTransition> transitions;
    bool operator==(const MotionSet &) const = default;
};

bool ValidMotionName(std::string_view name) noexcept;
void ValidateMotion(const MotionSet &set);
const MotionTransition *FindMotion(const MotionSet &set, std::string_view name) noexcept;
} // namespace prism::contracts
