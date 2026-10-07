#pragma once
#include "prism/animation/timeline.hpp"
#include "prism/contracts/panel_contour.hpp"
#include "prism/runtime/dsl_schema.hpp"
#include "prism/runtime/presentation.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/runtime/text_buffer.hpp"
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
    std::optional<GestureSpec> gesture;
    std::vector<StateRule> resolved_state_rules;
    std::vector<PropertyAssignment> state_values;
    Node *state_owner{};
    bool decorative{};
    VisualPresentation presentation{};
    Node *parent{};
    Style style{};
    std::shared_ptr<const contracts::Contour> contour_source;
    std::shared_ptr<const contracts::Contour> contour;
    std::optional<AttachedPanelRecipe> contour_recipe;
    std::optional<contracts::PanelContourSpec> contour_spec;
    std::optional<contracts::PanelContourRequest> contour_request;
    std::shared_ptr<const contracts::Contour> contour_prepared;
    std::optional<PopupPlacement> popup_placement;
    std::map<DslProperty, PropertyValue> properties;
    std::set<DslProperty> explicit_properties;
    std::vector<ThemeRef> theme_refs;
    std::uint64_t allowed_properties{UINT64_MAX};
    TextBuffer editor;
    std::vector<std::pair<std::size_t, contracts::LogicalRect>> text_cells;
    std::string text;
    std::string action;
    std::string icon;
    double value{0};
    NumberDomain number_domain;
    std::string slider_part;
    std::string popup_for;
    std::uint64_t popup_token{};
    contracts::NodeId popup_anchor;
    std::string scroll_part;
    double scroll_offset{}, scroll_content_height{};
    double scroll_speed{3};
    bool checked{false};
    std::string selected_key;
    std::string option_key;
    std::uint64_t control_revision{};
    std::unique_ptr<ControlValueSession> control_session;
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
        std::shared_ptr<const InputSnapshot> snapshot;
        bool submitted{};
        std::uint64_t gesture{};
        std::uint64_t control_revision{};
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
        std::uint64_t control_revision{};
    };

    struct Touch {
        contracts::InputSource source{};
        contracts::InputContactId contact{};
        contracts::NodeId captured{};
        std::string action;
        contracts::LogicalPoint position{};
        bool inside{};
        std::shared_ptr<const InputSnapshot> snapshot;
        bool submitted{};
        std::uint64_t gesture{};
    };

    struct Gesture {
        contracts::GestureEvent event;
        GestureSpec spec;
        std::string click_action;
        bool dragging{};
        bool begin_pending{};
        contracts::LogicalPoint begin_position{};
        std::uint64_t begin_time_ns{};
        bool update_pending{};
        std::uint64_t update_time_ns{};
        std::optional<contracts::GesturePhase> terminal;
    };

    struct SliderStream {
        contracts::NodeId node;
        std::string action;
        contracts::InputSource source;
        std::uint32_t key{}; // Zero is a pointer stream.
        contracts::LogicalRect track;
        std::uint64_t interaction{};
        std::uint64_t revision{};
        NumberDomain domain;
        ControlValueSession session{NumberDomain{}, 0.0};
        bool preview_pending{};
        std::optional<ControlValueEvent> terminal;
    };

    std::vector<SliderStream> sliders;
    std::uint64_t slider_sequence{};
    std::string clipboard;
    std::vector<Gesture> gestures;
    std::vector<Pointer> pointers;
    std::vector<Focus> focus;
    std::vector<KeyPress> keys;
    std::vector<Touch> touches;
    // Only active targets are revisited for pointer motion; idle nodes need no scan.
    std::vector<contracts::NodeId> active;
};

struct Scene::PopupSurfaceAdoption {
    PopupSurfaceIdentity identity;
    PopupSurfacePlan plan;
    std::shared_ptr<const InputSnapshot> trusted_input;
    std::map<std::uint32_t, double> scroll_offsets;
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

inline bool ValidEditorText(Kind kind, const PropertyValue &value)
{
    if (kind != Kind::TextField && kind != Kind::TextArea) {
        return true;
    }
    const auto *text = std::get_if<std::string>(&value);
    return text && TextBuffer::Valid(*text) &&
           (kind != Kind::TextField || text->find_first_of("\r\n") == std::string::npos);
}

inline bool ValidPropertyValue(DslProperty id, const PropertyValue &value)
{
    return ::prism::runtime::ValidPropertyValue(id, value);
}

} // namespace scene_detail
} // namespace prism::runtime
