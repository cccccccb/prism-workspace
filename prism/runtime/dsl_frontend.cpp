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
    if (!CorrectType(spec.type, value)) Error(line, "wrong value type for '" + std::string(spec.name) + "'");
    if (auto* number = std::get_if<double>(&value.data)) {
        if (!std::isfinite(*number) || *number < 0 || *number > 16384)
            Error(line, "invalid numeric value for '" + std::string(spec.name) + "'");
        switch (spec.id) {
            case DslProperty::Width: out.style.width = *number; return;
            case DslProperty::Height: out.style.height = *number; return;
            case DslProperty::Font: out.style.font_size = *number; return;
            case DslProperty::Spacing: out.style.spacing = *number; return;
            case DslProperty::Padding: out.style.padding = *number; return;
            case DslProperty::Radius: out.style.radius = *number; return;
            default: break;
        }
    }
    if (auto* color = std::get_if<ColorValue>(&value.data)) {
        if (spec.id == DslProperty::Background) out.style.background = UnpackColor(color->rgba);
        else out.style.foreground = UnpackColor(color->rgba);
        return;
    }
    if (spec.id == DslProperty::Clip) { out.style.clip = std::get<bool>(value.data); return; }
    if (spec.id == DslProperty::Action) { out.action = std::get<std::string>(value.data); return; }
    if (spec.id == DslProperty::Text) {
        if (auto* binding = std::get_if<BindingValue>(&value.data)) out.slot = binding->name;
        else out.text = std::get<std::string>(value.data);
        return;
    }
    if (spec.id == DslProperty::Source) {
        const auto& uri = std::get<std::string>(value.data);
        if (!resolve_image || uri.empty()) Error(line, "Image requires a resource resolver and URI");
        out.image = resolve_image(uri);
        if (!out.image) Error(line, "Image resource request failed: " + uri);
        return;
    }
    Error(line, "unsupported property");
}
Blueprint Convert(const SyntaxNode& node, const ResolveImage& resolve_image) {
    const auto* component = FindComponent(node.name);
    if (!component) Error(node.line, "unsupported client DSL component: " + node.name);
    if (!component->allows_children && !node.children.empty())
        Error(node.line, node.name + " cannot have children");
    Blueprint out;
    out.kind = component->kind;
    out.style.spacing = component->default_spacing;
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
    if (component->positional == DslProperty::Source && component->has_positional && !out.image)
        Error(node.line, "Image requires source");
    for (const auto& child : node.children) out.children.push_back(Convert(child, resolve_image));
    if (component->creates_label) {
        Blueprint label;
        label.kind = Kind::Text;
        label.text = std::move(out.text);
        label.slot = std::move(out.slot);
        label.style.font_size = out.style.font_size;
        label.style.foreground = out.style.foreground;
        out.children.push_back(std::move(label));
    }
    return out;
}
} // namespace

Blueprint ParseBlueprint(std::string_view source, ResolveImage resolve_image) {
    return Convert(ParseSyntax(source), resolve_image);
}
} // namespace prism::runtime
