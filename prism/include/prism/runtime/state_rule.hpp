#pragma once

#include "prism/runtime/property.hpp"
#include <vector>

namespace prism::runtime {

enum class StateCondition { Hovered, Pressed, Captured, Disabled, Focused, FocusVisible };

// The only v1 scope is the nearest enclosing InteractionTarget. References
// remain typed theme names until installation; no input is sent to a module.
struct StateRule {
    StateCondition condition;
    std::vector<PropertyAssignment> properties;
    std::vector<ThemeRef> theme_refs;

    bool operator==(const StateRule &) const = default;
};

constexpr bool ValidStateCondition(StateCondition condition)
{
    return condition >= StateCondition::Hovered && condition <= StateCondition::FocusVisible;
}

constexpr bool IsFocusCondition(StateCondition condition)
{
    return condition == StateCondition::Focused || condition == StateCondition::FocusVisible;
}

constexpr int StatePriority(StateCondition condition)
{
    switch (condition) {
    case StateCondition::Disabled:
        return 400;
    case StateCondition::Pressed:
        return 300;
    case StateCondition::Captured:
        return 200;
    case StateCondition::Hovered:
        return 100;
    case StateCondition::Focused:
    case StateCondition::FocusVisible:
        return 50;
    }
    return 0;
}

} // namespace prism::runtime
