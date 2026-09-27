#include "prism/runtime/dsl_schema.hpp"
#include <array>

namespace prism::runtime {
namespace {
constexpr std::array properties{
    PropertySpec{"width", DslProperty::Width, DslValueType::Number, Dirty::Layout,
                 StoredValueType::Number},
    PropertySpec{"height", DslProperty::Height, DslValueType::Number, Dirty::Layout,
                 StoredValueType::Number},
    PropertySpec{"font", DslProperty::Font, DslValueType::Number, Dirty::Layout | Dirty::Paint,
                 StoredValueType::Number, 0, 16384, false},
    PropertySpec{"spacing", DslProperty::Spacing, DslValueType::Number, Dirty::Layout,
                 StoredValueType::Number},
    PropertySpec{"background", DslProperty::Background, DslValueType::Color, Dirty::Paint,
                 StoredValueType::Color},
    PropertySpec{"foreground", DslProperty::Foreground, DslValueType::Color, Dirty::Paint,
                 StoredValueType::Color},
    PropertySpec{"padding", DslProperty::Padding, DslValueType::Number, Dirty::Layout,
                 StoredValueType::Number},
    PropertySpec{"cornerRadius", DslProperty::Radius, DslValueType::Number, Dirty::Paint,
                 StoredValueType::Number, 0, 256},
    PropertySpec{"clip", DslProperty::Clip, DslValueType::Boolean, Dirty::Paint,
                 StoredValueType::Boolean},
    PropertySpec{"action", DslProperty::Action, DslValueType::String, Dirty::None,
                 StoredValueType::String},
    PropertySpec{"text", DslProperty::Text, DslValueType::Text, Dirty::Layout | Dirty::Paint,
                 StoredValueType::String},
    PropertySpec{"source", DslProperty::Source, DslValueType::String, Dirty::Layout | Dirty::Paint,
                 StoredValueType::Resource},
    PropertySpec{"align", DslProperty::Align, DslValueType::String, Dirty::Layout,
                 StoredValueType::String},
    PropertySpec{"justify", DslProperty::Justify, DslValueType::String, Dirty::Layout,
                 StoredValueType::String},
    PropertySpec{"anchor", DslProperty::Anchor, DslValueType::String, Dirty::Layout,
                 StoredValueType::String},
    PropertySpec{"overflow", DslProperty::Overflow, DslValueType::String, Dirty::Paint,
                 StoredValueType::String},
    PropertySpec{"flex", DslProperty::Flex, DslValueType::Number, Dirty::Layout,
                 StoredValueType::Number},
    PropertySpec{"inset", DslProperty::Inset, DslValueType::Number, Dirty::Layout,
                 StoredValueType::Number},
    PropertySpec{"paddingX", DslProperty::PaddingX, DslValueType::Number, Dirty::Layout,
                 StoredValueType::Number},
    PropertySpec{"paddingY", DslProperty::PaddingY, DslValueType::Number, Dirty::Layout,
                 StoredValueType::Number},
    PropertySpec{"borderWidth", DslProperty::BorderWidth, DslValueType::Number, Dirty::Paint,
                 StoredValueType::Number},
    PropertySpec{"borderColor", DslProperty::BorderColor, DslValueType::Color, Dirty::Paint,
                 StoredValueType::Color},
    PropertySpec{"shadowBlur", DslProperty::ShadowBlur, DslValueType::Number, Dirty::Paint,
                 StoredValueType::Number},
    PropertySpec{"shadowY", DslProperty::ShadowY, DslValueType::Number, Dirty::Paint,
                 StoredValueType::Number, -16384},
    PropertySpec{"shadowColor", DslProperty::ShadowColor, DslValueType::Color, Dirty::Paint,
                 StoredValueType::Color},
    PropertySpec{"innerShadowBlur", DslProperty::InnerShadowBlur, DslValueType::Number,
                 Dirty::Paint, StoredValueType::Number},
    PropertySpec{"innerShadowColor", DslProperty::InnerShadowColor, DslValueType::Color,
                 Dirty::Paint, StoredValueType::Color},
    PropertySpec{"icon", DslProperty::Icon, DslValueType::String, Dirty::Paint,
                 StoredValueType::String},
    PropertySpec{"value", DslProperty::Value, DslValueType::Number, Dirty::Paint,
                 StoredValueType::Number, 0, 1},
    PropertySpec{"checked", DslProperty::Checked, DslValueType::Boolean, Dirty::Paint,
                 StoredValueType::Boolean},
    PropertySpec{"fit", DslProperty::ImageFit, DslValueType::String, Dirty::Paint,
                 StoredValueType::String},
    PropertySpec{"backdropBlur", DslProperty::BackdropBlur, DslValueType::Number, Dirty::Composite,
                 StoredValueType::Number, 0, 48},
    PropertySpec{"material", DslProperty::Material, DslValueType::String,
                 Dirty::Paint | Dirty::Composite, StoredValueType::String},
    PropertySpec{"inputShape", DslProperty::InputShape, DslValueType::String, Dirty::Composite,
                 StoredValueType::String},
    PropertySpec{"innerShadowY", DslProperty::InnerShadowY, DslValueType::Number, Dirty::Paint,
                 StoredValueType::Number, -16384},
    PropertySpec{"visible", DslProperty::Visible, DslValueType::Boolean,
                 Dirty::Layout | Dirty::Paint | Dirty::Composite, StoredValueType::Boolean},
};
constexpr auto size = PropertyBit(DslProperty::Width) | PropertyBit(DslProperty::Height) |
                      PropertyBit(DslProperty::Flex) | PropertyBit(DslProperty::Inset) |
                      PropertyBit(DslProperty::Anchor) | PropertyBit(DslProperty::Visible);
constexpr auto effects =
    PropertyBit(DslProperty::BorderWidth) | PropertyBit(DslProperty::BorderColor) |
    PropertyBit(DslProperty::ShadowBlur) | PropertyBit(DslProperty::ShadowY) |
    PropertyBit(DslProperty::ShadowColor) | PropertyBit(DslProperty::InnerShadowBlur) |
    PropertyBit(DslProperty::InnerShadowColor) | PropertyBit(DslProperty::BackdropBlur) |
    PropertyBit(DslProperty::Material) | PropertyBit(DslProperty::InputShape) |
    PropertyBit(DslProperty::InnerShadowY);
constexpr auto container =
    size | PropertyBit(DslProperty::Spacing) | PropertyBit(DslProperty::Background) |
    PropertyBit(DslProperty::Padding) | PropertyBit(DslProperty::Radius) |
    PropertyBit(DslProperty::Clip) | PropertyBit(DslProperty::PaddingX) |
    PropertyBit(DslProperty::PaddingY) | PropertyBit(DslProperty::Align) |
    PropertyBit(DslProperty::Justify) | PropertyBit(DslProperty::Overflow) | effects;
constexpr std::array components{
    ComponentSpec{"HStack", Kind::Row, DslProperty::Text, false, true, false, container, 8},
    ComponentSpec{"VStack", Kind::Column, DslProperty::Text, false, true, false, container, 8},
    ComponentSpec{"Card", Kind::Box, DslProperty::Text, false, true, false, container, 8},
    ComponentSpec{"Text", Kind::Text, DslProperty::Text, true, false, false,
                  size | PropertyBit(DslProperty::Text) | PropertyBit(DslProperty::Font) |
                      PropertyBit(DslProperty::Foreground) | PropertyBit(DslProperty::Background) |
                      PropertyBit(DslProperty::Clip) | PropertyBit(DslProperty::Overflow),
                  0},
    ComponentSpec{"Button", Kind::Box, DslProperty::Text, true, false, true,
                  container | PropertyBit(DslProperty::Text) | PropertyBit(DslProperty::Action) |
                      PropertyBit(DslProperty::Foreground) | PropertyBit(DslProperty::Font),
                  0},
    ComponentSpec{"Image", Kind::Image, DslProperty::Source, true, false, false,
                  size | PropertyBit(DslProperty::Source) | PropertyBit(DslProperty::Clip) |
                      PropertyBit(DslProperty::Radius) | PropertyBit(DslProperty::ImageFit),
                  0},
    ComponentSpec{"Icon", Kind::Icon, DslProperty::Icon, true, false, false,
                  size | PropertyBit(DslProperty::Icon) | PropertyBit(DslProperty::Foreground), 0},
    ComponentSpec{"IconButton", Kind::IconButton, DslProperty::Icon, true, false, false,
                  container | PropertyBit(DslProperty::Icon) |
                      PropertyBit(DslProperty::Foreground) | PropertyBit(DslProperty::Action),
                  0},
    ComponentSpec{"Separator", Kind::Separator, DslProperty::Text, false, false, false,
                  size | PropertyBit(DslProperty::Background), 0},
    ComponentSpec{
        "Progress", Kind::Progress, DslProperty::Value, false, false, false,
        container | PropertyBit(DslProperty::Value) | PropertyBit(DslProperty::Foreground), 0},
    ComponentSpec{"Toggle", Kind::Toggle, DslProperty::Checked, false, false, false,
                  container | PropertyBit(DslProperty::Checked) |
                      PropertyBit(DslProperty::Foreground) | PropertyBit(DslProperty::Action),
                  0},
};
} // namespace

const PropertySpec *FindProperty(std::string_view name)
{
    for (const auto &property : properties) {
        if (property.name == name) {
            return &property;
        }
    }
    return nullptr;
}

const PropertySpec *FindProperty(DslProperty id)
{
    for (const auto &property : properties) {
        if (property.id == id) {
            return &property;
        }
    }
    return nullptr;
}

const ComponentSpec *FindComponent(std::string_view name)
{
    for (const auto &component : components) {
        if (component.name == name) {
            return &component;
        }
    }
    return nullptr;
}
} // namespace prism::runtime
