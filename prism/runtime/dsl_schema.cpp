#include "prism/runtime/dsl_schema.hpp"
#include <array>

namespace prism::runtime {
namespace {
constexpr std::array properties{
    PropertySpec{"width", DslProperty::Width, DslValueType::Number, Dirty::Layout},
    PropertySpec{"height", DslProperty::Height, DslValueType::Number, Dirty::Layout},
    PropertySpec{"font", DslProperty::Font, DslValueType::Number, Dirty::Layout | Dirty::Paint},
    PropertySpec{"spacing", DslProperty::Spacing, DslValueType::Number, Dirty::Layout},
    PropertySpec{"background", DslProperty::Background, DslValueType::Color, Dirty::Paint},
    PropertySpec{"foreground", DslProperty::Foreground, DslValueType::Color, Dirty::Paint},
    PropertySpec{"padding", DslProperty::Padding, DslValueType::Number, Dirty::Layout},
    PropertySpec{"cornerRadius", DslProperty::Radius, DslValueType::Number, Dirty::Paint},
    PropertySpec{"clip", DslProperty::Clip, DslValueType::Boolean, Dirty::Paint},
    PropertySpec{"action", DslProperty::Action, DslValueType::String, Dirty::None},
    PropertySpec{"text", DslProperty::Text, DslValueType::Text, Dirty::Layout | Dirty::Paint},
    PropertySpec{"source", DslProperty::Source, DslValueType::String, Dirty::Layout | Dirty::Paint},
};
constexpr auto size = PropertyBit(DslProperty::Width) | PropertyBit(DslProperty::Height);
constexpr auto container = size | PropertyBit(DslProperty::Spacing) | PropertyBit(DslProperty::Background) |
    PropertyBit(DslProperty::Padding) | PropertyBit(DslProperty::Radius) | PropertyBit(DslProperty::Clip);
constexpr std::array components{
    ComponentSpec{"HStack", Kind::Row, DslProperty::Text, false, true, false, container, 8},
    ComponentSpec{"VStack", Kind::Column, DslProperty::Text, false, true, false, container, 8},
    ComponentSpec{"Card", Kind::Box, DslProperty::Text, false, true, false, container, 8},
    ComponentSpec{"Text", Kind::Text, DslProperty::Text, true, false, false,
        size | PropertyBit(DslProperty::Text) | PropertyBit(DslProperty::Font) |
        PropertyBit(DslProperty::Foreground) | PropertyBit(DslProperty::Background) | PropertyBit(DslProperty::Clip), 0},
    ComponentSpec{"Button", Kind::Box, DslProperty::Text, true, false, true,
        container | PropertyBit(DslProperty::Text) | PropertyBit(DslProperty::Action) |
        PropertyBit(DslProperty::Foreground) | PropertyBit(DslProperty::Font), 0},
    ComponentSpec{"Image", Kind::Image, DslProperty::Source, true, false, false,
        size | PropertyBit(DslProperty::Source) | PropertyBit(DslProperty::Clip), 0},
};
} // namespace
const PropertySpec* FindProperty(std::string_view name) {
    for (const auto& property : properties) if (property.name == name) return &property;
    return nullptr;
}
const ComponentSpec* FindComponent(std::string_view name) {
    for (const auto& component : components) if (component.name == name) return &component;
    return nullptr;
}
} // namespace prism::runtime
