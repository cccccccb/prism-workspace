#pragma once

#include "prism/contracts/contour.hpp"
#include "prism/runtime/contour_recipe.hpp"
#include "prism/runtime/gesture_spec.hpp"
#include <optional>

#include "prism/runtime/property.hpp"
#include "prism/runtime/state_rule.hpp"
#include "prism/runtime/transition_spec.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace prism::runtime {

enum class Kind {
    Row,
    Column,
    Box,
    Text,
    Image,
    Icon,
    IconButton,
    Progress,
    Toggle,
    Separator,
    InteractionTarget,
    Visual,
    TextField,
    TextArea,
    Checkbox,
    RadioGroup,
    SegmentGroup,
    Radio,
    Segment,
    Slider,
    ScrollView,
    Popup,
    Menu,
    MenuItem,
    MenuBack
};

constexpr bool IsPopupKind(Kind kind)
{
    return kind == Kind::Popup || kind == Kind::Menu;
}

constexpr bool IsMenuRow(Kind kind)
{
    return kind == Kind::MenuItem || kind == Kind::MenuBack;
}

constexpr bool IsChoiceGroup(Kind kind)
{
    return kind == Kind::RadioGroup || kind == Kind::SegmentGroup;
}

constexpr bool IsChoiceOption(Kind kind)
{
    return kind == Kind::Radio || kind == Kind::Segment;
}

constexpr bool IsControlTarget(Kind kind)
{
    return kind == Kind::Checkbox || kind == Kind::Slider || IsChoiceOption(kind);
}

constexpr bool IsInteractionOwner(Kind kind)
{
    return kind == Kind::InteractionTarget || IsControlTarget(kind) || IsMenuRow(kind);
}

struct Blueprint {
    Kind kind{Kind::Box};
    std::optional<contracts::Contour> contour;
    std::optional<AttachedPanelRecipe> contour_recipe;
    std::string region;
    bool region_mounted{false};
    std::vector<PropertyAssignment> properties;
    std::vector<PropertyBinding> bindings;
    std::vector<ThemeRef> theme_refs;
    std::vector<TransitionSpec> transitions;
    std::vector<StateRule> state_rules;
    std::optional<GestureSpec> gesture;
    std::uint64_t allowed_properties{UINT64_MAX};
    std::vector<Blueprint> children;
};

} // namespace prism::runtime
