#pragma once
#include "prism/contracts/display_list.hpp"
#include <cstdint>
#include <string>
#include <variant>

namespace prism::runtime {
enum class Dirty : std::uint8_t { None = 0, Layout = 1, Paint = 2, Composite = 4 };

constexpr Dirty operator|(Dirty a, Dirty b)
{
    return static_cast<Dirty>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

constexpr bool Has(Dirty value, Dirty bit)
{
    return (static_cast<unsigned>(value) & static_cast<unsigned>(bit)) != 0;
}
enum class DslProperty {
    Width,
    Height,
    Font,
    Spacing,
    Background,
    Foreground,
    Padding,
    Radius,
    Clip,
    Action,
    Text,
    Source,
    Align,
    Justify,
    Flex,
    Inset,
    PaddingX,
    PaddingY,
    Anchor,
    Overflow,
    BorderWidth,
    BorderColor,
    ShadowBlur,
    ShadowY,
    ShadowColor,
    InnerShadowBlur,
    InnerShadowColor,
    Icon,
    Value,
    Checked,
    ImageFit,
    BackdropBlur,
    Material,
    InputShape,
    InnerShadowY,
    Visible
};
using PropertyValue =
    std::variant<double, bool, std::string, contracts::Color, contracts::ResourceId>;

struct PropertyAssignment {
    DslProperty id;
    PropertyValue value;
};

struct PropertyBinding {
    std::string name;
    DslProperty target;
};

struct ThemeRef {
    std::string name;
    DslProperty target;
};
} // namespace prism::runtime
