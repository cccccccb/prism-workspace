#pragma once

#include "prism/animation/timeline.hpp"
#include "prism/runtime/property.hpp"
#include <cstdint>
#include <string>

namespace prism::runtime {

// Immutable, typed transition metadata compiled from one DSL modifier.
struct TransitionSpec {
    DslProperty property;
    std::uint32_t duration_ms;
    animation::Easing easing;
    std::string motion; // Empty for a literal timing; otherwise a theme semantic name.

    friend bool operator==(const TransitionSpec &, const TransitionSpec &) = default;
};

} // namespace prism::runtime
