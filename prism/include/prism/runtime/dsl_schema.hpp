#pragma once
#include "prism/runtime/scene.hpp"
#include <string_view>

namespace prism::runtime {
enum class DslValueType { Number, Color, Boolean, String, Text };
enum class DslProperty {
    Width, Height, Font, Spacing, Background, Foreground,
    Padding, Radius, Clip, Action, Text, Source
};
struct PropertySpec {
    std::string_view name;
    DslProperty id;
    DslValueType type;
    Dirty affects;
};
struct ComponentSpec {
    std::string_view name;
    Kind kind;
    DslProperty positional;
    bool has_positional;
    bool allows_children;
    bool creates_label;
    std::uint64_t allowed_properties;
    double default_spacing;
};
constexpr std::uint64_t PropertyBit(DslProperty id) { return 1ULL << static_cast<unsigned>(id); }
const PropertySpec* FindProperty(std::string_view name);
const ComponentSpec* FindComponent(std::string_view name);
} // namespace prism::runtime
