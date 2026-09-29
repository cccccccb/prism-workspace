#pragma once
#include "prism/animation/timeline.hpp"
#include "prism/runtime/dsl_schema.hpp"
#include "prism/runtime/presentation.hpp"
#include "prism/runtime/scene.hpp"
#include <algorithm>
#include <cmath>
#include <set>

namespace prism::runtime {
struct Scene::Node {
    contracts::NodeId id{};
    Kind kind{Kind::Box};
    std::string region;
    bool region_mounted{false};
    std::vector<PropertyBinding> bindings;
    std::vector<TransitionSpec> transitions;
    std::vector<StateRule> state_rules;
    std::vector<StateRule> resolved_state_rules;
    std::vector<PropertyAssignment> state_values;
    Node *state_owner{};
    bool decorative{};
    VisualPresentation presentation{};
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
    bool enabled{true};
    InteractionState interaction{};
    std::vector<std::unique_ptr<Node>> children;
};

struct Scene::InputState {
    struct Pointer {
        contracts::InputSource source{};
        contracts::NodeId hovered{}, captured{};
        std::string action;
        contracts::LogicalPoint position{};
        bool inside{};
    };

    struct Focus {
        std::uint64_t seat{};
        contracts::NodeId node{};
        bool visible{};
    };

    struct KeyPress {
        contracts::InputSource source{};
        contracts::NodeId node{};
        std::uint32_t key{};
        std::string action;
    };

    std::vector<Pointer> pointers;
    std::vector<Focus> focus;
    std::vector<KeyPress> keys;
    // Only active targets are revisited for pointer motion; idle nodes need no scan.
    std::vector<contracts::NodeId> active;
};

struct Scene::AnimationState {
    using Key = std::pair<std::uint32_t, DslProperty>;

    struct Track {
        Track(const animation::AnimationClock &clock, contracts::NodeId node_id,
              DslProperty property_id, PropertyValue from_value, PropertyValue target_value)
            : node(node_id), property(property_id), from(std::move(from_value)),
              target(std::move(target_value)), presented(from), progress(clock)
        {
        }

        contracts::NodeId node;
        DslProperty property;
        PropertyValue from;
        PropertyValue target;
        PropertyValue presented;
        animation::ScalarTimeline progress;

        PropertyValue Interpolate(double progress_value) const;
    };

    explicit AnimationState(const animation::AnimationClock &value) : clock(value)
    {
    }

    const animation::AnimationClock &clock;
    std::map<Key, std::unique_ptr<Track>> tracks;
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
