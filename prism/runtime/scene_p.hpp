#pragma once
#include "prism/runtime/dsl_schema.hpp"
#include "prism/runtime/scene.hpp"
#include <algorithm>
#include <cmath>
#include <set>

namespace prism::runtime {
struct Scene::Node {
    contracts::NodeId id{};
    Kind kind{Kind::Box};
    Node *parent{};
    Style style{};
    std::map<DslProperty, PropertyValue> properties;
    std::set<DslProperty> explicit_properties;
    std::vector<ThemeRef> theme_refs;
    std::uint64_t allowed_properties{UINT64_MAX};
    std::string text;
    std::string action;
    std::string icon;
    double value{0};
    bool checked{false};
    contracts::ResourceId image{};
    contracts::LogicalSize intrinsic_size{};
    bool image_ready{false};
    contracts::LogicalRect bounds{};
    ShapedText shaped{};
    std::uint64_t revision{1};
    std::vector<std::unique_ptr<Node>> children;
};

namespace scene_detail {
inline bool Inside(contracts::LogicalRect r, contracts::LogicalPoint p)
{
    return p.x >= r.x && p.y >= r.y && p.x < r.x + r.width && p.y < r.y + r.height;
}

inline bool ValidSize(contracts::LogicalSize s)
{
    return std::isfinite(s.width) && std::isfinite(s.height) && s.width > 0 && s.height > 0 &&
           s.width <= 16384 && s.height <= 16384;
}

inline bool ValidPropertyValue(DslProperty id, const PropertyValue &value)
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
                "grid",     "music",  "settings",  "folder",  "terminal", "play",   "pause",
                "previous", "next",   "volume",    "wifi",    "battery",  "search", "sun",
                "moon",     "power",  "check",     "chevron", "refresh",  "cpu",    "memory",
                "heart",    "layers", "rectangle", "drop",    "wifi-off", "error"};
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
} // namespace scene_detail
} // namespace prism::runtime
