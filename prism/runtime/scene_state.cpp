#include "prism/runtime/scene_snapshot.hpp"
#include "prism/runtime/theme_tokens.hpp"
#include "scene_p.hpp"
#include <new>
#include <stdexcept>

namespace prism::runtime {
namespace {
bool Matches(StateCondition condition, const InteractionState &state)
{
    switch (condition) {
    case StateCondition::Hovered:
        return state.hovered;
    case StateCondition::Pressed:
        return state.pressed;
    case StateCondition::Dragging:
        return state.dragging;
    case StateCondition::Captured:
        return state.captured;
    case StateCondition::Disabled:
        return !state.enabled;
    case StateCondition::Focused:
        return state.focused;
    case StateCondition::FocusVisible:
        return state.focusVisible;
    }
    return false;
}

bool ForbiddenDecorationProperty(DslProperty property)
{
    return property == DslProperty::Action || property == DslProperty::Material ||
           property == DslProperty::BackdropBlur || property == DslProperty::InputShape;
}
} // namespace

void Scene::PrepareNodeStates(Node &node, std::vector<StateRule> rules)
{
    std::map<DslProperty, std::vector<StateCondition>> writers;
    node.state_rules = std::move(rules);
    for (const auto &rule : node.state_rules) {
        if (!ValidStateCondition(rule.condition) ||
            (rule.properties.empty() && rule.theme_refs.empty())) {
            throw std::invalid_argument("Invalid Blueprint state condition or empty rule");
        }

        StateRule resolved{rule.condition, rule.properties, {}};
        for (const auto &ref : rule.theme_refs) {
            if (!theme_) {
                throw std::invalid_argument("State theme reference requires a theme snapshot");
            }
            const auto value = ResolveThemeToken(*theme_, ref.name);
            if (!value) {
                throw std::invalid_argument("Unknown state theme token: " + ref.name);
            }
            resolved.properties.push_back({ref.target, *value});
        }
        for (const auto &assignment : resolved.properties) {
            const auto property = assignment.id;
            if (!SupportsState(node.kind, property) ||
                !(node.allowed_properties & PropertyBit(property)) ||
                !scene_detail::ValidPropertyValue(property, assignment.value)) {
                throw std::invalid_argument("Invalid Blueprint state property");
            }
            auto &conditions = writers[property];
            for (const auto previous : conditions) {
                if (previous == rule.condition || IsFocusCondition(previous) ||
                    IsFocusCondition(rule.condition)) {
                    throw std::invalid_argument("Conflicting Blueprint state writers");
                }
            }
            conditions.push_back(rule.condition);
        }
        node.resolved_state_rules.push_back(std::move(resolved));
    }
    for (const auto &[property, conditions] : writers) {
        (void)conditions;
        node.state_values.push_back({property, CurrentProperty(node, property)});
    }
}

void Scene::ValidateInteractionTree(const Node &node, const Node *owner, bool decorative) const
{
    if (node.kind == Kind::Visual) {
        if (!node.parent) {
            throw std::invalid_argument("Visual cannot be a scene root");
        }
        decorative = true;
    }
    if (node.kind == Kind::InteractionTarget) {
        if (decorative) {
            throw std::invalid_argument("InteractionTarget cannot be a Visual descendant");
        }
        owner = &node;
    }
    if (!node.state_rules.empty() && (!decorative || !owner)) {
        throw std::invalid_argument("State rules require Visual and an enclosing target");
    }
    if (decorative && !node.region.empty()) {
        throw std::invalid_argument("Visual cannot contain an asynchronous region");
    }

    for (const auto property : node.explicit_properties) {
        if ((decorative && ForbiddenDecorationProperty(property)) ||
            (property >= DslProperty::TranslateX && property <= DslProperty::Opacity &&
             node.kind != Kind::Visual)) {
            throw std::invalid_argument("Invalid decoration property scope");
        }
    }
    for (const auto &binding : node.bindings) {
        if ((decorative && ForbiddenDecorationProperty(binding.target)) ||
            (binding.target >= DslProperty::TranslateX && binding.target <= DslProperty::Opacity &&
             node.kind != Kind::Visual)) {
            throw std::invalid_argument("Invalid decoration binding scope");
        }
    }
    for (const auto &child : node.children) {
        ValidateInteractionTree(*child, owner, decorative);
    }
}

void Scene::BindInteractionTree(Node &node, Node *owner, bool decorative) noexcept
{
    if (node.kind == Kind::InteractionTarget) {
        owner = &node;
    }
    decorative = decorative || node.kind == Kind::Visual;
    node.state_owner = owner;
    node.decorative = decorative;
    for (auto &child : node.children) {
        BindInteractionTree(*child, owner, decorative);
    }
}

void Scene::PrepareInteractionTree()
{
    ValidateInteractionTree(*root_, nullptr, false);
    BindInteractionTree(*root_, nullptr, false);
    ResolveStateTargets(AnimationNowNs(), false);
}

bool Scene::IsStateProperty(const Node &node, DslProperty property) const noexcept
{
    return std::any_of(
        node.state_values.begin(), node.state_values.end(),
        [property](const PropertyAssignment &value) { return value.id == property; });
}

PropertyValue Scene::EffectiveProperty(const Node &node, DslProperty property) const
{
    for (const auto &value : node.state_values) {
        if (value.id == property) {
            return value.value;
        }
    }
    return CurrentProperty(node, property);
}

void Scene::ResolveNodeStateTargets(Node &node, std::uint64_t now, bool animate) noexcept
{
    if (!node.state_owner || node.state_values.empty()) {
        return;
    }
    const auto state = State(node.state_owner->id);
    const bool visible = IsVisible(node);
    for (auto &target : node.state_values) {
        auto value = CurrentProperty(node, target.id);
        int priority = 0;
        for (const auto &rule : node.resolved_state_rules) {
            if (!Matches(rule.condition, state) || StatePriority(rule.condition) <= priority) {
                continue;
            }
            for (const auto &assignment : rule.properties) {
                if (assignment.id == target.id) {
                    value = assignment.value;
                    priority = StatePriority(rule.condition);
                    break;
                }
            }
        }
        if (value == target.value) {
            continue;
        }

        bool animated = false;
        if (animate && visible) {
            try {
                animated = RetargetPresentation(node, target.id, target.value, value, now);
            } catch (const std::bad_alloc &) {
                animation_state_->tracks.erase({node.id.index, target.id});
            }
        } else if (animation_state_) {
            animation_state_->tracks.erase({node.id.index, target.id});
        }
        target.value = std::move(value);
        if (visible && !animated) {
            ++node.revision;
            Invalidate(Dirty::Paint);
            RecordAnimationSample(now);
        }
    }
}

void Scene::ResolveStateTargets(std::uint64_t now, bool animate) noexcept
{
    if (!state_styles_dirty_) {
        return;
    }
    for (auto *node : nodes_) {
        if (node && !node->state_values.empty()) {
            ResolveNodeStateTargets(*node, now, animate);
        }
    }
    state_styles_dirty_ = false;
}

void Scene::ResolveInteractionStyles()
{
    ResolveStateTargets(AnimationNowNs(), true);
}

void Scene::ApplySnapshotProperty(SnapshotNode &snapshot, DslProperty property,
                                  const PropertyValue &value) const
{
    switch (property) {
    case DslProperty::Background:
        snapshot.style.background = std::get<contracts::Color>(value);
        break;
    case DslProperty::Foreground:
        snapshot.style.foreground = std::get<contracts::Color>(value);
        break;
    case DslProperty::Value:
        snapshot.value = std::get<double>(value);
        break;
    case DslProperty::TranslateX:
        snapshot.presentation.translate_x = std::get<double>(value);
        break;
    case DslProperty::TranslateY:
        snapshot.presentation.translate_y = std::get<double>(value);
        break;
    case DslProperty::ScaleX:
        snapshot.presentation.scale_x = std::get<double>(value);
        break;
    case DslProperty::ScaleY:
        snapshot.presentation.scale_y = std::get<double>(value);
        break;
    case DslProperty::OriginX:
        snapshot.presentation.origin_x = std::get<double>(value);
        break;
    case DslProperty::OriginY:
        snapshot.presentation.origin_y = std::get<double>(value);
        break;
    case DslProperty::Opacity:
        snapshot.presentation.opacity = std::get<double>(value);
        break;
    default:
        break;
    }
}
} // namespace prism::runtime
