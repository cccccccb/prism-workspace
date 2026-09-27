#include "prism/runtime/dsl_frontend.hpp"
#include "prepared_component_p.hpp"
#include "prism/compiler/error.hpp"
#include "prism/runtime/dsl_schema.hpp"
#include "prism/runtime/dsl_syntax.hpp"
#include <string>
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

class ComponentCompiler {
public:
    explicit ComponentCompiler(const ComponentSource &source) : source_(source)
    {
    }

    PreparedNode Convert(const SyntaxNode &node)
    {
        AddNode(node.line);
        const auto *component = FindComponent(node.name);
        if (!component) {
            Error(node.line, "unsupported client DSL component: " + node.name);
        }
        if (!component->allows_children && !node.children.empty()) {
            Error(node.line, node.name + " cannot have children");
        }

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
        if (component->positional == DslProperty::Source && component->has_positional &&
            !seen.contains(DslProperty::Source)) {
            Error(node.line, "Image requires source");
        }
        CountEffects(out);

        for (const auto &child : node.children) {
            out.children.push_back(Convert(child));
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

    ComponentCompiler compiler(source_info);
    auto root = compiler.Convert(syntax);
    auto data = std::make_shared<PreparedComponent::Data>();
    data->source = std::move(source_info);
    data->root = std::move(root);
    data->images = compiler.TakeImages();
    data->source_bytes = source.size();
    data->node_count = compiler.NodeCount();
    return PreparedComponent(std::move(data));
}

Blueprint ParseBlueprint(std::string_view source, ResolveImage resolve_image)
{
    return LinkComponent(PrepareComponent(source), std::move(resolve_image));
}

} // namespace prism::runtime
