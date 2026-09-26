#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/dsl_schema.hpp"
#include "prism/runtime/dsl_syntax.hpp"
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace prism::runtime {
namespace {
[[noreturn]] void Error(int line, std::string message) {
    throw std::runtime_error("DSL line " + std::to_string(line) + ": " + std::move(message));
}
contracts::Color UnpackColor(std::uint32_t rgba) {
    return {static_cast<std::uint8_t>(rgba >> 24), static_cast<std::uint8_t>(rgba >> 16),
            static_cast<std::uint8_t>(rgba >> 8), static_cast<std::uint8_t>(rgba)};
}
bool CorrectType(DslValueType type, const SyntaxValue& value) {
    switch (type) {
        case DslValueType::Number: return std::holds_alternative<double>(value.data);
        case DslValueType::Color: return std::holds_alternative<ColorValue>(value.data);
        case DslValueType::Boolean: return std::holds_alternative<bool>(value.data);
        case DslValueType::String: return std::holds_alternative<std::string>(value.data);
        case DslValueType::Text: return std::holds_alternative<std::string>(value.data) ||
                                        std::holds_alternative<BindingValue>(value.data);
    }
    return false;
}
void Apply(Blueprint& out, const PropertySpec& spec, const SyntaxValue& value,
           int line, const ResolveImage& resolve_image) {
    if (auto* binding = std::get_if<BindingValue>(&value.data)) {
        out.bindings.push_back({binding->name, spec.id});
        return;
    }
    if (!CorrectType(spec.type, value)) Error(line, "wrong value type for '" + std::string(spec.name) + "'");
    if (auto* number = std::get_if<double>(&value.data)) {
        if (!std::isfinite(*number) || *number < spec.min_value || *number > spec.max_value ||
            (!spec.allow_zero && *number == 0))
            Error(line, "invalid numeric value for '" + std::string(spec.name) + "'");
        out.properties.push_back({spec.id, *number});
        return;
    }
    if (auto* color = std::get_if<ColorValue>(&value.data)) {
        out.properties.push_back({spec.id, UnpackColor(color->rgba)});
        return;
    }
    if (spec.id == DslProperty::Source) {
        const auto& uri = std::get<std::string>(value.data);
        if (!resolve_image || uri.empty()) Error(line, "Image requires a resource resolver and URI");
        auto image = resolve_image(uri);
        if (!image) Error(line, "Image resource request failed: " + uri);
        out.properties.push_back({spec.id, image});
        return;
    }
    if (auto* boolean = std::get_if<bool>(&value.data)) out.properties.push_back({spec.id, *boolean});
    else if (auto* string = std::get_if<std::string>(&value.data)) out.properties.push_back({spec.id, *string});
    else Error(line, "unsupported property");
}
Blueprint Convert(const SyntaxNode& node, const ResolveImage& resolve_image) {
    const auto* component = FindComponent(node.name);
    if (!component) Error(node.line, "unsupported client DSL component: " + node.name);
    if (!component->allows_children && !node.children.empty())
        Error(node.line, node.name + " cannot have children");
    Blueprint out;
    out.kind = component->kind;
    out.allowed_properties = component->allowed_properties;
    if (component->default_spacing > 0)
        out.properties.push_back({DslProperty::Spacing, component->default_spacing});
    std::unordered_set<DslProperty> seen;
    auto assign = [&](std::string_view name, const SyntaxValue& value, int line) {
        const auto* spec = FindProperty(name);
        if (!spec) Error(line, "unknown property: " + std::string(name));
        if (!seen.insert(spec->id).second) Error(line, "duplicate property: " + std::string(name));
        if ((component->allowed_properties & PropertyBit(spec->id)) == 0)
            Error(line, "property not allowed on " + node.name + ": " + std::string(name));
        Apply(out, *spec, value, line, resolve_image);
    };
    unsigned positional_count = 0;
    for (const auto& argument : node.arguments) {
        if (argument.name.empty()) {
            if (!component->has_positional) Error(argument.line, node.name + " has no positional argument");
            ++positional_count;
            const auto* spec = positional_count == 2 && component->creates_label
                ? FindProperty("action")
                : component->positional == DslProperty::Source ? FindProperty("source") : FindProperty("text");
            if (positional_count > (component->creates_label ? 2U : 1U))
                Error(argument.line, "too many positional arguments for " + node.name);
            assign(spec->name, argument.value, argument.line);
        } else assign(argument.name, argument.value, argument.line);
    }
    for (const auto& modifier : node.modifiers) {
        if (modifier.name == "clip" && modifier.arguments.empty()) {
            assign("clip", SyntaxValue{{true}}, modifier.line);
            continue;
        }
        if (modifier.name != "padding" && modifier.name != "cornerRadius" && modifier.name != "clip")
            Error(modifier.line, "unsupported client DSL modifier: " + modifier.name);
        if (modifier.arguments.size() != 1 || !modifier.arguments.front().name.empty())
            Error(modifier.line, "modifier requires one positional value: " + modifier.name);
        assign(modifier.name, modifier.arguments.front().value, modifier.line);
    }
    if (component->positional == DslProperty::Source && component->has_positional &&
        !seen.contains(DslProperty::Source))
        Error(node.line, "Image requires source");
    for (const auto& child : node.children) out.children.push_back(Convert(child, resolve_image));
    if (component->creates_label) {
        Blueprint label;
        label.kind = Kind::Text;
        if (const auto* text = FindComponent("Text")) label.allowed_properties = text->allowed_properties;
        for (auto it = out.properties.begin(); it != out.properties.end();) {
            if (it->id == DslProperty::Text) {
                label.properties.push_back(std::move(*it));
                it = out.properties.erase(it);
            } else {
                if (it->id == DslProperty::Font || it->id == DslProperty::Foreground)
                    label.properties.push_back(*it);
                ++it;
            }
        }
        for (auto it = out.bindings.begin(); it != out.bindings.end();) {
            if (it->target == DslProperty::Text) {
                label.bindings.push_back(std::move(*it));
                it = out.bindings.erase(it);
            } else ++it;
        }
        out.children.push_back(std::move(label));
    }
    return out;
}
} // namespace

Blueprint ParseBlueprint(std::string_view source, ResolveImage resolve_image) {
    return Convert(ParseSyntax(source), resolve_image);
}
} // namespace prism::runtime
