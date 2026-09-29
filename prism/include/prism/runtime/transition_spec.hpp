#pragma once

#include "prism/animation/timeline.hpp"
#include "prism/runtime/property.hpp"
#include <cstdint>

namespace prism::runtime {

// Immutable, typed transition metadata compiled from one DSL modifier.
struct TransitionSpec {
    DslProperty property;
    std::uint32_t duration_ms;
    animation::Easing easing;

    friend bool operator==(const TransitionSpec &, const TransitionSpec &) = default;
};

} // namespace prism::runtime
