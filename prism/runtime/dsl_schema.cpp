#include "prism/runtime/dsl_schema.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>

namespace prism::runtime {
namespace {
constexpr std::array properties{
    PropertySpec{"tooltipFor", DslProperty::TooltipFor, DslValueType::String, Dirty::None,
                 StoredValueType::String},
    PropertySpec{"tooltipDelayMs", DslProperty::TooltipDelayMs, DslValueType::Number, Dirty::None,
                 StoredValueType::Number, 0, 10000},
    PropertySpec{"popupFor", DslProperty::PopupFor, DslValueType::String, Dirty::None,
                 StoredValueType::String},
    PropertySpec{"scrollSpeed", DslProperty::ScrollSpeed, DslValueType::Number, Dirty::None,
                 StoredValueType::Number, 0.1, 32},
    PropertySpec{"scrollPart", DslProperty::ScrollPart, DslValueType::String, Dirty::None,
                 StoredValueType::String},
    PropertySpec{"minimum", DslProperty::Minimum, DslValueType::Number, Dirty::None,
                 StoredValueType::Number, -1e12, 1e12},
    PropertySpec{"maximum", DslProperty::Maximum, DslValueType::Number, Dirty::None,
                 StoredValueType::Number, -1e12, 1e12},
    PropertySpec{"step", DslProperty::Step, DslValueType::Number, Dirty::None,
                 StoredValueType::Number, 0, 2e12},
    PropertySpec{"sliderPart", DslProperty::SliderPart, DslValueType::String, Dirty::None,
                 StoredValueType::String},
    PropertySpec{"selectedKey", DslProperty::SelectedKey, DslValueType::String, Dirty::Paint,
                 StoredValueType::String},
    PropertySpec{"key", DslProperty::OptionKey, DslValueType::String, Dirty::None,
                 StoredValueType::String},
    PropertySpec{"lineHeight", DslProperty::LineHeight, DslValueType::Number,
                 Dirty::Layout | Dirty::Paint, StoredValueType::Number, 0, 512},
    PropertySpec{"enabled", DslProperty::Enabled, DslValueType::Boolean, Dirty::Paint,
                 StoredValueType::Boolean},
    PropertySpec{"minViewportWidth", DslProperty::MinViewportWidth, DslValueType::Number,
                 Dirty::Layout | Dirty::Paint | Dirty::Composite, StoredValueType::Number},
    PropertySpec{"maxViewportWidth", DslProperty::MaxViewportWidth, DslValueType::Number,
                 Dirty::Layout | Dirty::Paint | Dirty::Composite, StoredValueType::Number},
    PropertySpec{"minViewportHeight", DslProperty::MinViewportHeight, DslValueType::Number,
                 Dirty::Layout | Dirty::Paint | Dirty::Composite, StoredValueType::Number},
    PropertySpec{"maxViewportHeight", DslProperty::MaxViewportHeight, DslValueType::Number,
                 Dirty::Layout | Dirty::Paint | Dirty::Composite, StoredValueType::Number},
    PropertySpec{"width", DslProperty::Width, DslValueType::Number, Dirty::Layout,
                 StoredValueType::Number},
    PropertySpec{"height", DslProperty::Height, DslValueType::Number, Dirty::Layout,
                 StoredValueType::Number},
    PropertySpec{"font", DslProperty::Font, DslValueType::Number, Dirty::Layout | Dirty::Paint,
                 StoredValueType::Number, 0, 16384, false},
    PropertySpec{"spacing", DslProperty::Spacing, DslValueType::Number, Dirty::Layout,
                 StoredValueType::Number},
    PropertySpec{"background", DslProperty::Background, DslValueType::Color, Dirty::Paint,
                 StoredValueType::Color, 0, 16384, true, KindBit(Kind::Visual)},
    PropertySpec{"foreground", DslProperty::Foreground, DslValueType::Color, Dirty::Paint,
                 StoredValueType::Color, 0, 16384, true,
                 KindBit(Kind::Text) | KindBit(Kind::Icon) | KindBit(Kind::IconButton) |
                     KindBit(Kind::Progress) | KindBit(Kind::Toggle)},
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
                 StoredValueType::Number, -1e12, 1e12, true, KindBit(Kind::Progress)},
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
    PropertySpec{"translateX", DslProperty::TranslateX, DslValueType::Number, Dirty::Paint,
                 StoredValueType::Number, -8192, 8192, true, KindBit(Kind::Visual)},
    PropertySpec{"translateY", DslProperty::TranslateY, DslValueType::Number, Dirty::Paint,
                 StoredValueType::Number, -8192, 8192, true, KindBit(Kind::Visual)},
    PropertySpec{"scaleX", DslProperty::ScaleX, DslValueType::Number, Dirty::Paint,
                 StoredValueType::Number, .01, 8, false, KindBit(Kind::Visual)},
    PropertySpec{"scaleY", DslProperty::ScaleY, DslValueType::Number, Dirty::Paint,
                 StoredValueType::Number, .01, 8, false, KindBit(Kind::Visual)},
    PropertySpec{"originX", DslProperty::OriginX, DslValueType::Number, Dirty::Paint,
                 StoredValueType::Number, 0, 1},
    PropertySpec{"originY", DslProperty::OriginY, DslValueType::Number, Dirty::Paint,
                 StoredValueType::Number, 0, 1},
    PropertySpec{"opacity", DslProperty::Opacity, DslValueType::Number, Dirty::Paint,
                 StoredValueType::Number, 0, 1, true, KindBit(Kind::Visual)},
};
constexpr auto size =
    PropertyBit(DslProperty::Width) | PropertyBit(DslProperty::Height) |
    PropertyBit(DslProperty::Flex) | PropertyBit(DslProperty::Inset) |
    PropertyBit(DslProperty::Anchor) | PropertyBit(DslProperty::Visible) |
    PropertyBit(DslProperty::Enabled) | PropertyBit(DslProperty::MinViewportWidth) |
    PropertyBit(DslProperty::MaxViewportWidth) | PropertyBit(DslProperty::MinViewportHeight) |
    PropertyBit(DslProperty::MaxViewportHeight);
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
constexpr auto visual =
    (container & ~(PropertyBit(DslProperty::Material) | PropertyBit(DslProperty::BackdropBlur) |
                   PropertyBit(DslProperty::InputShape))) |
    PropertyBit(DslProperty::TranslateX) | PropertyBit(DslProperty::TranslateY) |
    PropertyBit(DslProperty::ScaleX) | PropertyBit(DslProperty::ScaleY) |
    PropertyBit(DslProperty::OriginX) | PropertyBit(DslProperty::OriginY) |
    PropertyBit(DslProperty::Opacity) | PropertyBit(DslProperty::SliderPart) |
    PropertyBit(DslProperty::ScrollPart);
constexpr std::array components{
    ComponentSpec{"Tooltip", Kind::Tooltip, DslProperty::TooltipFor, true, true, false,
                  container | PropertyBit(DslProperty::TooltipFor) |
                      PropertyBit(DslProperty::TooltipDelayMs),
                  0},
    ComponentSpec{"Menu", Kind::Menu, DslProperty::PopupFor, true, true, false,
                  container | PropertyBit(DslProperty::PopupFor), 0},
    ComponentSpec{"MenuItem", Kind::MenuItem, DslProperty::Action, false, true, false,
                  container | PropertyBit(DslProperty::Action), 0},
    ComponentSpec{"MenuBack", Kind::MenuBack, DslProperty::Text, false, true, false, container, 0},
    ComponentSpec{"Popup", Kind::Popup, DslProperty::PopupFor, true, true, false,
                  container | PropertyBit(DslProperty::PopupFor), 0},
    ComponentSpec{
        "ScrollView", Kind::ScrollView, DslProperty::Text, false, true, false,
        (container & ~(PropertyBit(DslProperty::Padding) | PropertyBit(DslProperty::PaddingX) |
                       PropertyBit(DslProperty::PaddingY) | PropertyBit(DslProperty::Spacing) |
                       PropertyBit(DslProperty::Align) | PropertyBit(DslProperty::Justify) |
                       PropertyBit(DslProperty::Clip) | PropertyBit(DslProperty::Overflow))) |
            PropertyBit(DslProperty::ScrollSpeed),
        0},
    ComponentSpec{"Slider", Kind::Slider, DslProperty::Value, false, true, false,
                  container | PropertyBit(DslProperty::Value) | PropertyBit(DslProperty::Minimum) |
                      PropertyBit(DslProperty::Maximum) | PropertyBit(DslProperty::Step) |
                      PropertyBit(DslProperty::Action),
                  0},
    ComponentSpec{"HStack", Kind::Row, DslProperty::Text, false, true, false, container, 8},
    ComponentSpec{"VStack", Kind::Column, DslProperty::Text, false, true, false, container, 8},
    ComponentSpec{"Card", Kind::Box, DslProperty::Text, false, true, false, container, 8},
    ComponentSpec{"TextField", Kind::TextField, DslProperty::Text, true, false, false,
                  container | PropertyBit(DslProperty::Text) | PropertyBit(DslProperty::Action) |
                      PropertyBit(DslProperty::Font) | PropertyBit(DslProperty::LineHeight) |
                      PropertyBit(DslProperty::Foreground),
                  0},
    ComponentSpec{"TextArea", Kind::TextArea, DslProperty::Text, true, false, false,
                  container | PropertyBit(DslProperty::Text) | PropertyBit(DslProperty::Action) |
                      PropertyBit(DslProperty::Font) | PropertyBit(DslProperty::LineHeight) |
                      PropertyBit(DslProperty::Foreground),
                  0},
    ComponentSpec{"Text", Kind::Text, DslProperty::Text, true, false, false,
                  size | PropertyBit(DslProperty::Text) | PropertyBit(DslProperty::Font) |
                      PropertyBit(DslProperty::LineHeight) | PropertyBit(DslProperty::Foreground) |
                      PropertyBit(DslProperty::Background) | PropertyBit(DslProperty::Clip) |
                      PropertyBit(DslProperty::Overflow),
                  0},
    ComponentSpec{"Button", Kind::Box, DslProperty::Text, true, false, true,
                  container | PropertyBit(DslProperty::Text) | PropertyBit(DslProperty::Action) |
                      PropertyBit(DslProperty::Foreground) | PropertyBit(DslProperty::Font) |
                      PropertyBit(DslProperty::LineHeight),
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
    ComponentSpec{
        "RadioGroup", Kind::RadioGroup, DslProperty::Text, false, true, false,
        container | PropertyBit(DslProperty::SelectedKey) | PropertyBit(DslProperty::Action), 8},
    ComponentSpec{
        "SegmentGroup", Kind::SegmentGroup, DslProperty::Text, false, true, false,
        container | PropertyBit(DslProperty::SelectedKey) | PropertyBit(DslProperty::Action), 0},
    ComponentSpec{"Radio", Kind::Radio, DslProperty::OptionKey, true, true, false,
                  container | PropertyBit(DslProperty::OptionKey), 0},
    ComponentSpec{"Segment", Kind::Segment, DslProperty::OptionKey, true, true, false,
                  container | PropertyBit(DslProperty::OptionKey), 0},
    ComponentSpec{"Checkbox", Kind::Checkbox, DslProperty::Checked, false, true, false,
                  container | PropertyBit(DslProperty::Checked) | PropertyBit(DslProperty::Action),
                  0},
    ComponentSpec{"InteractionTarget", Kind::InteractionTarget, DslProperty::Text, false, true,
                  false, container | PropertyBit(DslProperty::Action), 0},
    ComponentSpec{"Visual", Kind::Visual, DslProperty::Text, false, true, false, visual, 0},
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

bool SupportsTransition(Kind kind, DslProperty property)
{
    const auto *spec = FindProperty(property);
    if (!spec || kind < Kind::Row || kind > Kind::Visual) {
        return false;
    }
    return (spec->transition_kinds & KindBit(kind)) != 0;
}

bool SupportsState(Kind kind, DslProperty property)
{
    if (property == DslProperty::Value) {
        return false;
    }
    if (kind == Kind::Visual &&
        (property == DslProperty::OriginX || property == DslProperty::OriginY)) {
        return true;
    }
    return SupportsTransition(kind, property);
}

bool SupportsContour(Kind kind)
{
    switch (kind) {
    case Kind::Box:
    case Kind::Row:
    case Kind::Column:
    case Kind::Visual:
    case Kind::InteractionTarget:
    case Kind::Popup:
    case Kind::Menu:
    case Kind::MenuItem:
    case Kind::MenuBack:
        return true;
    default:
        return false;
    }
}

bool ValidPropertyValue(DslProperty id, const PropertyValue &value)
{
    const auto *spec = FindProperty(id);
    if (!spec) {
        return false;
    }
    switch (spec->stored_type) {
    case StoredValueType::Number: {
        const auto *number = std::get_if<double>(&value);
        return number && std::isfinite(*number) && *number >= spec->min_value &&
               *number <= spec->max_value && (spec->allow_zero || *number != 0);
    }
    case StoredValueType::Color:
        return std::holds_alternative<contracts::Color>(value);
    case StoredValueType::Boolean:
        return std::holds_alternative<bool>(value);
    case StoredValueType::String: {
        const auto *text = std::get_if<std::string>(&value);
        if (!text) {
            return false;
        }
        if (id == DslProperty::OptionKey || id == DslProperty::SelectedKey ||
            id == DslProperty::PopupFor || id == DslProperty::TooltipFor) {
            return text->size() <= 128 && text->find('\0') == std::string::npos &&
                   (id == DslProperty::SelectedKey || !text->empty());
        }
        if (id == DslProperty::ScrollPart) {
            return *text == "track" || *text == "thumb";
        }
        if (id == DslProperty::SliderPart) {
            return *text == "track" || *text == "fill" || *text == "thumb";
        }
        if (id == DslProperty::Align) {
            return *text == "start" || *text == "center" || *text == "end" || *text == "stretch";
        }
        if (id == DslProperty::Justify) {
            return *text == "start" || *text == "center" || *text == "end" ||
                   *text == "spaceBetween";
        }
        if (id == DslProperty::Anchor) {
            return *text == "fill" || *text == "left" || *text == "center" || *text == "right";
        }
        if (id == DslProperty::Overflow) {
            return *text == "visible" || *text == "clip";
        }
        if (id == DslProperty::InputShape) {
            return *text == "visible" || *text == "bounds";
        }
        if (id == DslProperty::ImageFit) {
            return *text == "fill" || *text == "contain" || *text == "cover";
        }
        if (id == DslProperty::Icon) {
            constexpr std::string_view icons[] = {
                "grid",           "music",       "settings",     "folder",    "terminal",
                "play",           "pause",       "previous",     "next",      "volume",
                "wifi",           "battery",     "search",       "sun",       "moon",
                "power",          "check",       "chevron",      "refresh",   "cpu",
                "memory",         "heart",       "layers",       "rectangle", "drop",
                "wifi-off",       "error",       "fullscreen",   "restore",   "split-horizontal",
                "split-vertical", "document",    "document-add", "save",      "close",
                "arrow-left",     "arrow-right", "trash",        "info",      "monitor",
                "brush",          "activity",    "clock",        "repeat",    "heart-outline"};
            return std::find(std::begin(icons), std::end(icons), *text) != std::end(icons);
        }
        return true;
    }
    case StoredValueType::Resource:
        return std::holds_alternative<contracts::ResourceId>(value) &&
               static_cast<bool>(std::get<contracts::ResourceId>(value));
    }
    return false;
}
} // namespace prism::runtime
