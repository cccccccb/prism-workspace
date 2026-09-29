#pragma once
#include "prism/runtime/blueprint.hpp"
#include <string_view>

namespace prism::runtime {
enum class DslValueType { Number, Color, Boolean, String, Text };
enum class StoredValueType { Number, Color, Boolean, String, Resource };

struct PropertySpec {
    std::string_view name;
    DslProperty id;
    DslValueType type;
    Dirty affects;
    StoredValueType stored_type;
    double min_value{0};
    double max_value{16384};
    bool allow_zero{true};
    std::uint64_t transition_kinds{0};
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

constexpr std::uint64_t PropertyBit(DslProperty id)
{
    return 1ULL << static_cast<unsigned>(id);
}

constexpr std::uint64_t KindBit(Kind kind)
{
    return 1ULL << static_cast<unsigned>(kind);
}

const PropertySpec *FindProperty(std::string_view name);
const PropertySpec *FindProperty(DslProperty id);
const ComponentSpec *FindComponent(std::string_view name);
bool ValidPropertyValue(DslProperty id, const PropertyValue &value);
bool SupportsTransition(Kind kind, DslProperty property);
} // namespace prism::runtime
