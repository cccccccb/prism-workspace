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
    return ::prism::runtime::ValidPropertyValue(id, value);
}

} // namespace scene_detail
} // namespace prism::runtime
