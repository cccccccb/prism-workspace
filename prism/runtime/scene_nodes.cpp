#include "prism/runtime/theme_tokens.hpp"
#include "scene_contour_p.hpp"
#include "scene_p.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace prism::runtime {
std::unique_ptr<Scene::Node> Scene::MakeNode(Blueprint blueprint, std::size_t depth)
{
    auto children = std::move(blueprint.children);
    auto node = MakeShallowNode(std::move(blueprint), depth);
    for (auto &child : children) {
        auto created = MakeNode(std::move(child), depth + 1);
        created->parent = node.get();
        node->children.push_back(std::move(created));
    }
    return node;
}

std::unique_ptr<Scene::Node> Scene::MakeShallowNode(Blueprint blueprint, std::size_t depth)
{
    auto node = std::make_unique<Node>();
    if (nodes_.size() >= 8192 || depth > 64) {
        throw std::length_error("Scene node limit");
    }
    node->id = {static_cast<std::uint32_t>(nodes_.size()), 1};
    Node *raw = node.get();
    nodes_.push_back(raw);
    node_generations_.push_back(1);
    if (blueprint.kind < Kind::Row || blueprint.kind > Kind::Tooltip) {
        throw std::invalid_argument("Invalid Blueprint node kind");
    }
    node->kind = blueprint.kind;
    if (blueprint.contour_recipe) {
        if (blueprint.contour || !IsPopupKind(node->kind)) {
            throw std::invalid_argument(
                "Attached panel recipe requires an exclusive Popup/Menu contour");
        }
        node->contour_spec =
            ResolveContourRecipe(*blueprint.contour_recipe, theme_ ? &*theme_ : nullptr);
        node->contour_recipe = std::move(blueprint.contour_recipe);
    }
    if (blueprint.contour) {
        if (!SupportsContour(node->kind)) {
            throw std::invalid_argument("Contour is not supported by this component");
        }
        contracts::ValidateContour(*blueprint.contour);
        node->contour_source =
            std::make_shared<const contracts::Contour>(std::move(*blueprint.contour));
    }
    for (const auto &transition : blueprint.transitions) {
        if (!SupportsTransition(node->kind, transition.property) ||
            !(blueprint.allowed_properties & PropertyBit(transition.property)) ||
            transition.duration_ms > 10000 ||
            std::any_of(node->transitions.begin(), node->transitions.end(),
                        [&transition](const TransitionSpec &existing) {
                            return existing.property == transition.property;
                        })) {
            throw std::invalid_argument("Invalid Blueprint transition");
        }
        switch (transition.easing) {
        case animation::Easing::Linear:
        case animation::Easing::EaseInCubic:
        case animation::Easing::EaseOutCubic:
        case animation::Easing::EaseInOutCubic:
            break;
        default:
            throw std::invalid_argument("Invalid Blueprint transition easing");
        }
        if (!transition.motion.empty() &&
            (!contracts::ValidMotionName(transition.motion) || transition.duration_ms != 0 ||
             transition.easing != animation::Easing::Linear || !theme_ ||
             !contracts::FindMotion(theme_->motion, transition.motion))) {
            throw std::invalid_argument("Unknown theme motion: " + transition.motion);
        }
        node->transitions.push_back(transition);
    }
    node->region = std::move(blueprint.region);
    node->region_mounted = blueprint.region_mounted;
    if (!node->region.empty()) {
        if (node->kind != Kind::Box || !regions_.emplace(node->region, raw).second) {
            throw std::invalid_argument("Invalid or duplicate region wrapper");
        }
    }
    node->allowed_properties = blueprint.allowed_properties;
    if (theme_) {
        node->style.inner_shadow_y = theme_->controls.inner_shadow_y;
    }
    for (const auto &property : blueprint.properties) {
        if (property.id == DslProperty::Material) {
            if (!theme_) {
                throw std::invalid_argument("Material requires a theme snapshot");
            }
            const auto *name = std::get_if<std::string>(&property.value);
            if ((node->contour_source || node->contour_recipe) && name && *name == "window") {
                throw std::invalid_argument("Window material requires the standard frame contour");
            }
            const auto *material = name ? contracts::FindThemeMaterial(*theme_, *name) : nullptr;
            if (!material) {
                throw std::invalid_argument("Unknown theme material");
            }
            const PropertyAssignment values[] = {
                {DslProperty::Background, material->tint},
                {DslProperty::Radius, material->radius},
                {DslProperty::BackdropBlur, material->backdrop_blur},
                {DslProperty::BorderWidth, material->border_width},
                {DslProperty::BorderColor, material->border},
                {DslProperty::ShadowBlur, material->shadow_blur},
                {DslProperty::ShadowY, material->shadow_y},
                {DslProperty::ShadowColor, material->shadow},
                {DslProperty::InnerShadowBlur, material->inner_shadow_blur},
                {DslProperty::InnerShadowY, material->inner_shadow_y},
                {DslProperty::InnerShadowColor, material->inner_shadow},
                {DslProperty::InputShape,
                 std::string(material->input_shape == contracts::ThemeInputShape::Bounds
                                 ? "bounds"
                                 : "visible")}};
            for (const auto &value : values) {
                if (!(node->allowed_properties & PropertyBit(value.id)) ||
                    !scene_detail::ValidPropertyValue(value.id, value.value)) {
                    throw std::invalid_argument("Material is not supported by this component");
                }
                node->properties[value.id] = value.value;
                ApplyCachedProperty(*node, value.id, value.value);
            }
        }
    }
    for (auto &property : blueprint.properties) {
        if (!scene_detail::ValidPropertyValue(property.id, property.value) ||
            !(node->allowed_properties & PropertyBit(property.id))) {
            throw std::invalid_argument("Invalid Blueprint property");
        }
        node->properties[property.id] = property.value;
        node->explicit_properties.insert(property.id);
        ApplyCachedProperty(*node, property.id, property.value);
    }
    for (const auto &ref : blueprint.theme_refs) {
        if (!theme_) {
            throw std::invalid_argument("Theme reference requires a theme snapshot: " + ref.name);
        }
        const auto value = ResolveThemeToken(*theme_, ref.name);
        if (!value || !scene_detail::ValidPropertyValue(ref.target, *value) ||
            !(node->allowed_properties & PropertyBit(ref.target))) {
            throw std::invalid_argument("Invalid theme token for property: " + ref.name);
        }
        node->properties[ref.target] = *value;
        node->explicit_properties.insert(ref.target);
        ApplyCachedProperty(*node, ref.target, *value);
        node->theme_refs.push_back(ref);
    }
    for (auto &binding : blueprint.bindings) {
        if (binding.name.empty() || !FindProperty(binding.target) ||
            !(node->allowed_properties & PropertyBit(binding.target))) {
            throw std::invalid_argument("Invalid Blueprint binding");
        }
        bindings_[binding.name].push_back({raw, binding.target});
        node->explicit_properties.insert(binding.target);
    }
    node->bindings = std::move(blueprint.bindings);
    if (blueprint.gesture &&
        (blueprint.kind != Kind::InteractionTarget || !ValidGestureSpec(*blueprint.gesture))) {
        throw std::invalid_argument("Invalid Blueprint gesture");
    }
    if ((node->kind == Kind::TextField || node->kind == Kind::TextArea) &&
        (node->action.empty() || !scene_detail::ValidEditorText(node->kind, node->text))) {
        throw std::invalid_argument(
            "Text editor requires a nonempty action and bounded UTF-8 text");
    }
    node->gesture = std::move(blueprint.gesture);
    if ((node->contour_source || node->contour_recipe) &&
        (!node->slider_part.empty() || !node->scroll_part.empty())) {
        throw std::invalid_argument("Generated slider/scroll parts do not support Contour");
    }
    PrepareNodeStates(*node, std::move(blueprint.state_rules));
    return node;
}

Blueprint Scene::CurrentBlueprint(const Node &node) const
{
    Blueprint result;
    result.kind = node.kind;
    result.contour_recipe = node.contour_recipe;
    if (node.contour_source) {
        result.contour = *node.contour_source;
    }
    result.region = node.region;
    result.region_mounted = node.region_mounted;
    result.bindings = node.bindings;
    result.transitions = node.transitions;
    result.state_rules = node.state_rules;
    result.gesture = node.gesture;
    result.allowed_properties = node.allowed_properties;
    result.theme_refs = node.theme_refs;
    for (const auto id : node.explicit_properties) {
        if (std::any_of(node.theme_refs.begin(), node.theme_refs.end(),
                        [id](const ThemeRef &ref) { return ref.target == id; })) {
            continue;
        }
        auto value = CurrentProperty(node, id);
        if (scene_detail::ValidPropertyValue(id, value)) {
            result.properties.push_back({id, std::move(value)});
        }
    }
    for (const auto &child : node.children) {
        result.children.push_back(CurrentBlueprint(*child));
    }
    return result;
}

} // namespace prism::runtime
