#include "prism/runtime/dsl_frontend.hpp"
#include "load_plan_p.hpp"
#include "prepared_component_p.hpp"
#include "prism/compiler/error.hpp"
#include "prism/runtime/dsl_schema.hpp"
#include "prism/runtime/dsl_syntax.hpp"
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace prism::runtime {
namespace {
constexpr std::size_t max_source_bytes = 1024 * 1024;
constexpr std::size_t max_expanded_nodes = 8192;

contracts::Color UnpackColor(std::uint32_t rgba)
{
    return {static_cast<std::uint8_t>(rgba >> 24), static_cast<std::uint8_t>(rgba >> 16),
            static_cast<std::uint8_t>(rgba >> 8), static_cast<std::uint8_t>(rgba)};
}

bool CorrectType(DslValueType type, const SyntaxValue &value)
{
    switch (type) {
    case DslValueType::Number:
        return std::holds_alternative<double>(value.data);
    case DslValueType::Color:
        return std::holds_alternative<ColorValue>(value.data);
    case DslValueType::Boolean:
        return std::holds_alternative<bool>(value.data);
    case DslValueType::String:
        return std::holds_alternative<std::string>(value.data);
    case DslValueType::Text:
        return std::holds_alternative<std::string>(value.data) ||
               std::holds_alternative<BindingValue>(value.data);
    }
    return false;
}

std::optional<animation::Easing> ParseEasing(std::string_view name)
{
    if (name == "linear") {
        return animation::Easing::Linear;
    }
    if (name == "easeInCubic") {
        return animation::Easing::EaseInCubic;
    }
    if (name == "easeOutCubic") {
        return animation::Easing::EaseOutCubic;
    }
    if (name == "easeInOutCubic") {
        return animation::Easing::EaseInOutCubic;
    }
    return std::nullopt;
}

std::optional<StateCondition> ParseStateCondition(std::string_view name)
{
    if (name == "hovered") {
        return StateCondition::Hovered;
    }
    if (name == "pressed") {
        return StateCondition::Pressed;
    }
    if (name == "captured") {
        return StateCondition::Captured;
    }
    if (name == "disabled") {
        return StateCondition::Disabled;
    }
    if (name == "focused") {
        return StateCondition::Focused;
    }
    if (name == "focusVisible") {
        return StateCondition::FocusVisible;
    }
    return std::nullopt;
}

bool HasStateProperty(const StateRule &rule, DslProperty property)
{
    return std::any_of(
               rule.properties.begin(), rule.properties.end(),
               [property](const PropertyAssignment &item) { return item.id == property; }) ||
           std::any_of(rule.theme_refs.begin(), rule.theme_refs.end(),
                       [property](const ThemeRef &ref) { return ref.target == property; });
}

bool HasPreparedProperty(const PreparedNode &node, DslProperty property)
{
    return std::any_of(node.properties.begin(), node.properties.end(),
                       [property](const PreparedPropertyAssignment &item) {
                           return item.id == property;
                       }) ||
           std::any_of(
               node.bindings.begin(), node.bindings.end(),
               [property](const PropertyBinding &binding) { return binding.target == property; }) ||
           std::any_of(node.theme_refs.begin(), node.theme_refs.end(),
                       [property](const ThemeRef &ref) { return ref.target == property; });
}

class ComponentCompiler {
public:
    explicit ComponentCompiler(const ComponentSource &source) : source_(source)
    {
    }

    PreparedNode Convert(const SyntaxNode &node, bool root = true, bool visual = false,
                         bool target = false)
    {
        AddNode(node.line);
        const auto *component = FindComponent(node.name);
        if (!component) {
            Error(node.line, "unsupported client DSL component: " + node.name);
        }
        if (!component->allows_children && !node.children.empty()) {
            Error(node.line, node.name + " cannot have children");
        }
        if (component->kind == Kind::Visual && root) {
            Error(node.line, "Visual cannot be a component root");
        }
        if (component->kind == Kind::InteractionTarget) {
            if (visual) {
                Error(node.line, "Visual subtree cannot contain InteractionTarget");
            }
            target = true;
        }
        visual = visual || component->kind == Kind::Visual;

        PreparedNode out;
        out.kind = component->kind;
        out.allowed_properties = component->allowed_properties;
        out.line = node.line;
        if (component->default_spacing > 0) {
            out.properties.push_back({DslProperty::Spacing, component->default_spacing});
        }
        std::unordered_set<DslProperty> seen;
        AssignArguments(out, seen, *component, node);
        AssignModifiers(out, seen, *component, node);
        ValidatePresentation(out, visual, target);
        if (component->positional == DslProperty::Source && component->has_positional &&
            !seen.contains(DslProperty::Source)) {
            Error(node.line, "Image requires source");
        }
        CountEffects(out);

        for (const auto &child : node.children) {
            out.children.push_back(Convert(child, false, visual, target));
        }
        if (component->creates_label) {
            CreateLabel(out);
        }
        return out;
    }

    std::vector<PreparedImage> TakeImages()
    {
        return std::move(images_);
    }

    std::size_t NodeCount() const
    {
        return node_count_;
    }

private:
    [[noreturn]] void Error(int line, std::string message) const
    {
        throw LoadFailure({LoadStage::Semantic, source_, line, std::move(message)});
    }

    void AddNode(int line)
    {
        if (++node_count_ > max_expanded_nodes) {
            Error(line, "expanded component count exceeds limit of 8192");
        }
    }

    ImageReference Image(std::string uri, int line)
    {
        if (uri.empty()) {
            Error(line, "Image requires a non-empty URI");
        }
        if (const auto found = image_keys_.find(uri); found != image_keys_.end()) {
            return {found->second};
        }
        const auto key = images_.size();
        image_keys_.emplace(uri, key);
        images_.push_back({std::move(uri), line});
        return {key};
    }

    void Apply(PreparedNode &out, const PropertySpec &spec, const SyntaxValue &value, int line)
    {
        if (const auto *binding = std::get_if<BindingValue>(&value.data)) {
            if (spec.id == DslProperty::Material) {
                Error(line, "material is a static style reference");
            }
            if (binding->name.empty()) {
                Error(line, "empty binding");
            }
            out.bindings.push_back({binding->name, spec.id});
            return;
        }
        if (const auto *token = std::get_if<std::string>(&value.data);
            token && token->starts_with("@")) {
            if (token->size() == 1 ||
                (spec.type != DslValueType::Number && spec.type != DslValueType::Color)) {
                Error(line, "theme references require a numeric or color property");
            }
            out.theme_refs.push_back({token->substr(1), spec.id});
            return;
        }
        if (!CorrectType(spec.type, value)) {
            Error(line, "wrong value type for '" + std::string(spec.name) + "'");
        }

        if (const auto *number = std::get_if<double>(&value.data)) {
            if (!ValidPropertyValue(spec.id, PropertyValue{*number})) {
                Error(line, "invalid numeric value for '" + std::string(spec.name) + "'");
            }
            out.properties.push_back({spec.id, *number});
        } else if (const auto *color = std::get_if<ColorValue>(&value.data)) {
            out.properties.push_back({spec.id, UnpackColor(color->rgba)});
        } else if (spec.id == DslProperty::Source) {
            out.properties.push_back({spec.id, Image(std::get<std::string>(value.data), line)});
        } else if (const auto *boolean = std::get_if<bool>(&value.data)) {
            out.properties.push_back({spec.id, *boolean});
        } else if (const auto *text = std::get_if<std::string>(&value.data)) {
            if (!ValidPropertyValue(spec.id, PropertyValue{*text})) {
                Error(line, "invalid string value for '" + std::string(spec.name) + "'");
            }
            out.properties.push_back({spec.id, *text});
        } else {
            Error(line, "unsupported property");
        }
    }

    void AssignProperty(PreparedNode &out, std::unordered_set<DslProperty> &seen,
                        const ComponentSpec &component, const SyntaxNode &node,
                        std::string_view name, const SyntaxValue &value, int line)
    {
        const auto *spec = FindProperty(name);
        if (!spec) {
            Error(line, "unknown property: " + std::string(name));
        }
        if (!seen.insert(spec->id).second) {
            Error(line, "duplicate property: " + std::string(name));
        }
        if ((component.allowed_properties & PropertyBit(spec->id)) == 0) {
            Error(line, "property not allowed on " + node.name + ": " + std::string(name));
        }
        Apply(out, *spec, value, line);
    }

    void AssignArguments(PreparedNode &out, std::unordered_set<DslProperty> &seen,
                         const ComponentSpec &component, const SyntaxNode &node)
    {
        unsigned positional_count = 0;
        for (const auto &argument : node.arguments) {
            if (!argument.name.empty()) {
                AssignProperty(out, seen, component, node, argument.name, argument.value,
                               argument.line);
                continue;
            }
            if (!component.has_positional) {
                Error(argument.line, node.name + " has no positional argument");
            }
            ++positional_count;
            const bool supports_action =
                component.creates_label || component.kind == Kind::IconButton;
            if (positional_count > (supports_action ? 2U : 1U)) {
                Error(argument.line, "too many positional arguments for " + node.name);
            }
            const auto *spec =
                positional_count == 2 ? FindProperty("action") : FindProperty(component.positional);
            AssignProperty(out, seen, component, node, spec->name, argument.value, argument.line);
        }
    }

    void AssignModifiers(PreparedNode &out, std::unordered_set<DslProperty> &seen,
                         const ComponentSpec &component, const SyntaxNode &node)
    {
        for (const auto &modifier : node.modifiers) {
            if (modifier.name == "state") {
                AssignState(out, component, node, modifier);
                continue;
            }
            if (modifier.name == "transition") {
                AssignTransition(out, component, node, modifier);
                continue;
            }
            if (modifier.name == "clip" && modifier.arguments.empty()) {
                AssignProperty(out, seen, component, node, "clip", SyntaxValue{{true}},
                               modifier.line);
                continue;
            }
            if (!FindProperty(modifier.name)) {
                Error(modifier.line, "unsupported client DSL modifier: " + modifier.name);
            }
            if (modifier.arguments.size() != 1 || !modifier.arguments.front().name.empty()) {
                Error(modifier.line, "modifier requires one positional value: " + modifier.name);
            }
            AssignProperty(out, seen, component, node, modifier.name,
                           modifier.arguments.front().value, modifier.line);
        }
    }

    void AssignState(PreparedNode &out, const ComponentSpec &component, const SyntaxNode &node,
                     const SyntaxModifier &modifier)
    {
        const SyntaxArgument *when_arg = nullptr;
        const SyntaxArgument *scope_arg = nullptr;
        for (const auto &argument : modifier.arguments) {
            const SyntaxArgument **slot = nullptr;
            if (argument.name == "when") {
                slot = &when_arg;
            } else if (argument.name == "scope") {
                slot = &scope_arg;
            }
            if (slot) {
                if (*slot) {
                    Error(argument.line, "duplicate state argument: " + argument.name);
                }
                *slot = &argument;
            }
        }
        if (!when_arg || !scope_arg) {
            Error(modifier.line, "state requires when, scope and at least one property");
        }
        const auto *when = std::get_if<std::string>(&when_arg->value.data);
        const auto condition = when ? ParseStateCondition(*when) : std::nullopt;
        if (!condition) {
            Error(when_arg->line, "unknown state condition");
        }
        const auto *scope = std::get_if<std::string>(&scope_arg->value.data);
        if (!scope || *scope != "target") {
            Error(scope_arg->line, "state scope must be the literal target");
        }

        StateRule rule{*condition, {}, {}};
        std::unordered_set<DslProperty> seen;
        for (const auto &argument : modifier.arguments) {
            if (argument.name == "when" || argument.name == "scope") {
                continue;
            }
            AssignStateProperty(rule, seen, component, node, argument);
        }
        if (seen.empty()) {
            Error(modifier.line, "state requires at least one property");
        }
        for (const auto &existing : out.state_rules) {
            for (const auto property : seen) {
                if (!HasStateProperty(existing, property)) {
                    continue;
                }
                if (existing.condition == rule.condition) {
                    Error(modifier.line, "duplicate state condition property");
                }
                if (IsFocusCondition(existing.condition) || IsFocusCondition(rule.condition)) {
                    Error(modifier.line, "focus state property conflicts with another condition");
                }
            }
        }
        out.state_rules.push_back(std::move(rule));
    }

    void AssignStateProperty(StateRule &rule, std::unordered_set<DslProperty> &seen,
                             const ComponentSpec &component, const SyntaxNode &node,
                             const SyntaxArgument &argument)
    {
        const auto *spec = FindProperty(argument.name);
        if (!spec || !SupportsState(component.kind, spec->id) ||
            !(component.allowed_properties & PropertyBit(spec->id))) {
            Error(argument.line,
                  "state property not supported on " + node.name + ": " + argument.name);
        }
        if (!seen.insert(spec->id).second) {
            Error(argument.line, "duplicate state property: " + argument.name);
        }
        if (std::holds_alternative<BindingValue>(argument.value.data)) {
            Error(argument.line, "state values require literals or theme references");
        }

        PreparedNode value;
        Apply(value, *spec, argument.value, argument.line);
        if (!value.theme_refs.empty()) {
            rule.theme_refs.push_back(std::move(value.theme_refs.front()));
            return;
        }
        const auto &prepared = value.properties.front().value;
        if (const auto *number = std::get_if<double>(&prepared)) {
            rule.properties.push_back({spec->id, *number});
        } else if (const auto *color = std::get_if<contracts::Color>(&prepared)) {
            rule.properties.push_back({spec->id, *color});
        } else {
            Error(argument.line, "state values require numeric or color values");
        }
    }

    void ValidatePresentation(const PreparedNode &node, bool visual, bool target)
    {
        if (!node.state_rules.empty() && (!visual || !target)) {
            Error(node.line, "state requires a Visual subtree inside an InteractionTarget");
        }
        if (!visual) {
            return;
        }
        constexpr DslProperty excluded[]{DslProperty::Action, DslProperty::Material,
                                         DslProperty::BackdropBlur, DslProperty::InputShape};
        for (const auto property : excluded) {
            if (HasPreparedProperty(node, property)) {
                Error(node.line,
                      "Visual subtree cannot declare " + std::string(FindProperty(property)->name));
            }
        }
    }

    void AssignTransition(PreparedNode &out, const ComponentSpec &component, const SyntaxNode &node,
                          const SyntaxModifier &modifier)
    {
        const SyntaxArgument *property_arg = nullptr;
        const SyntaxArgument *duration_arg = nullptr;
        const SyntaxArgument *easing_arg = nullptr;

        for (const auto &argument : modifier.arguments) {
            const SyntaxArgument **slot = nullptr;
            if (argument.name == "property") {
                slot = &property_arg;
            } else if (argument.name == "durationMs") {
                slot = &duration_arg;
            } else if (argument.name == "easing") {
                slot = &easing_arg;
            } else {
                Error(argument.line, "unknown transition argument: " + argument.name);
            }
            if (*slot) {
                Error(argument.line, "duplicate transition argument: " + argument.name);
            }
            *slot = &argument;
        }
        if (!property_arg || !duration_arg || !easing_arg || modifier.arguments.size() != 3) {
            Error(modifier.line, "transition requires property, durationMs and easing");
        }

        const auto *name = std::get_if<std::string>(&property_arg->value.data);
        const auto *property = name ? FindProperty(*name) : nullptr;
        if (!property || !SupportsTransition(component.kind, property->id) ||
            !(component.allowed_properties & PropertyBit(property->id))) {
            Error(property_arg->line, "property cannot transition on " + node.name);
        }

        const auto *milliseconds = std::get_if<double>(&duration_arg->value.data);
        if (!milliseconds || !std::isfinite(*milliseconds) || *milliseconds < 0 ||
            *milliseconds > 10000 || std::trunc(*milliseconds) != *milliseconds) {
            Error(duration_arg->line, "transition durationMs must be an integer from 0 to 10000");
        }

        const auto *easing_name = std::get_if<std::string>(&easing_arg->value.data);
        const auto easing = easing_name ? ParseEasing(*easing_name) : std::nullopt;
        if (!easing) {
            Error(easing_arg->line, "unknown transition easing");
        }

        for (const auto &existing : out.transitions) {
            if (existing.property == property->id) {
                Error(modifier.line, "duplicate transition property: " + *name);
            }
        }
        out.transitions.push_back(
            {property->id, static_cast<std::uint32_t>(*milliseconds), *easing});
    }

    void CreateLabel(PreparedNode &out)
    {
        AddNode(out.line);
        PreparedNode label;
        label.kind = Kind::Text;
        label.line = out.line;
        label.allowed_properties = FindComponent("Text")->allowed_properties;
        for (auto it = out.properties.begin(); it != out.properties.end();) {
            if (it->id == DslProperty::Text) {
                label.properties.push_back(std::move(*it));
                it = out.properties.erase(it);
            } else {
                if (it->id == DslProperty::Font || it->id == DslProperty::Foreground) {
                    label.properties.push_back(*it);
                }
                ++it;
            }
        }
        for (auto it = out.bindings.begin(); it != out.bindings.end();) {
            if (it->target == DslProperty::Text) {
                label.bindings.push_back(std::move(*it));
                it = out.bindings.erase(it);
            } else {
                if (it->target == DslProperty::Font || it->target == DslProperty::Foreground) {
                    label.bindings.push_back(*it);
                }
                ++it;
            }
        }
        for (const auto &ref : out.theme_refs) {
            if (ref.target == DslProperty::Font || ref.target == DslProperty::Foreground) {
                label.theme_refs.push_back(ref);
            }
        }
        out.children.push_back(std::move(label));
    }

    void CountEffects(const PreparedNode &node)
    {
        bool effect = false;
        for (const auto &property : node.properties) {
            if (property.id == DslProperty::BackdropBlur && std::get<double>(property.value) > 0) {
                effect = true;
            }
        }
        for (const auto &binding : node.bindings) {
            if (binding.target == DslProperty::BackdropBlur) {
                effect = true;
            }
        }
        for (const auto &ref : node.theme_refs) {
            if (ref.target == DslProperty::BackdropBlur) {
                effect = true;
            }
        }
        if (effect && ++effect_regions_ > 8) {
            Error(node.line, "DSL surface effect region limit is 8");
        }
    }

    const ComponentSource &source_;
    std::vector<PreparedImage> images_;
    std::unordered_map<std::string, std::size_t> image_keys_;
    std::size_t node_count_{0};
    std::size_t effect_regions_{0};
};
} // namespace

PreparedComponent PrepareVisualSyntax(const SyntaxNode &syntax, ComponentSource source_info,
                                      std::size_t source_bytes)
{
    ComponentCompiler compiler(source_info);
    auto root = compiler.Convert(syntax);
    return PreparedComponentAccess::Make(std::move(source_info), std::move(root),
                                         compiler.TakeImages(), source_bytes, compiler.NodeCount());
}

PreparedComponent PrepareComponent(std::string_view source, ComponentSource source_info)
{
    if (source.size() > max_source_bytes) {
        throw LoadFailure({LoadStage::Semantic, source_info, 0, "DSL source exceeds 1 MiB limit"});
    }
    SyntaxNode syntax;
    try {
        syntax = ParseSyntax(source);
    } catch (const compiler::CompilerError &error) {
        throw LoadFailure({LoadStage::Syntax, source_info, error.Line(), error.Message()});
    }

    return PrepareVisualSyntax(syntax, std::move(source_info), source.size());
}

Blueprint ParseBlueprint(std::string_view source, ResolveImage resolve_image)
{
    return LinkComponent(PrepareComponent(source), std::move(resolve_image));
}

} // namespace prism::runtime
