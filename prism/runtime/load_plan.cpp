#include "load_plan_p.hpp"
#include "prism/compiler/error.hpp"
#include "prism/compiler/lexer.hpp"
#include "prism/runtime/dsl_schema.hpp"
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace prism::runtime {

[[noreturn]] void LoadSemanticError(const ComponentSource &source, int line, std::string message)
{
    throw LoadFailure({LoadStage::Semantic, source, line, std::move(message)});
}

SyntaxNode ReadLoadSyntax(std::string_view text, const ComponentSource &source)
{
    if (text.size() > kMaxLoadFileBytes) {
        LoadSemanticError(source, 0, "DSL source exceeds 1 MiB limit");
    }
    try {
        return ParseSyntax(text);
    } catch (const compiler::CompilerError &error) {
        throw LoadFailure({LoadStage::Syntax, source, error.Line(), error.Message()});
    }
}

bool IsLoadIdentifier(std::string_view name)
{
    if (name.empty() || name.size() > 128) {
        return false;
    }
    const auto letter = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    if (!letter(name.front())) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [letter](char c) {
        return letter(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
    });
}

std::filesystem::path LoadRelativePath(std::string_view name, const ComponentSource &source,
                                       int line)
{
    const std::filesystem::path path(name);
    if (name.empty() || name.size() > 4096 || name.find('\0') != std::string_view::npos ||
        path.is_absolute() || path.has_root_path()) {
        LoadSemanticError(source, line, "source path must be package-root-relative");
    }
    for (const auto &part : path) {
        if (part.empty() || part == "." || part == "..") {
            LoadSemanticError(source, line,
                              "source path cannot contain empty, '.' or '..' segments");
        }
    }
    return path;
}

namespace {
class Fields {
public:
    Fields(const SyntaxNode &node, const ComponentSource &source,
           std::initializer_list<std::string_view> allowed)
        : node_(node), source_(source)
    {
        if (!node.modifiers.empty()) {
            LoadSemanticError(source, node.modifiers.front().line,
                              "load declarations do not accept modifiers");
        }
        for (const auto &argument : node.arguments) {
            if (argument.name.empty() ||
                std::find(allowed.begin(), allowed.end(), argument.name) == allowed.end()) {
                LoadSemanticError(source, argument.line,
                                  "unknown or positional load field: " + argument.name);
            }
            if (!fields_.emplace(argument.name, &argument).second) {
                LoadSemanticError(source, argument.line, "duplicate load field: " + argument.name);
            }
        }
    }

    const SyntaxArgument &Get(std::string_view name) const
    {
        const auto *field = Optional(name);
        if (!field) {
            LoadSemanticError(source_, node_.line, "missing load field: " + std::string(name));
        }
        return *field;
    }

    const SyntaxArgument *Optional(std::string_view name) const
    {
        const auto found = fields_.find(std::string(name));
        return found == fields_.end() ? nullptr : found->second;
    }

    std::string Text(std::string_view name) const
    {
        const auto &field = Get(name);
        const auto *value = std::get_if<std::string>(&field.value.data);
        if (!value || value->empty()) {
            LoadSemanticError(source_, field.line,
                              "load field requires a nonempty string: " + std::string(name));
        }
        return *value;
    }

private:
    const SyntaxNode &node_;
    const ComponentSource &source_;
    std::unordered_map<std::string, const SyntaxArgument *> fields_;
};

void RequireLeaf(const SyntaxNode &node, const ComponentSource &source)
{
    if (!node.children.empty()) {
        LoadSemanticError(source, node.line, node.name + " cannot have children");
    }
}

LoadBinding ParseBinding(const SyntaxNode &node, const ComponentSource &source)
{
    RequireLeaf(node, source);
    Fields fields(node, source, {"name", "type", "initial"});
    LoadBinding binding;
    binding.name = fields.Text("name");
    binding.line = node.line;
    if (!IsLoadIdentifier(binding.name)) {
        LoadSemanticError(source, node.line, "invalid binding name: " + binding.name);
    }
    const auto type = fields.Text("type");
    const auto &initial = fields.Get("initial");
    bool correct = false;
    if (type == "string") {
        binding.type = LoadBindingType::String;
        if (const auto *value = std::get_if<std::string>(&initial.value.data)) {
            binding.initial = *value;
            correct = true;
        }
    } else if (type == "number") {
        binding.type = LoadBindingType::Number;
        if (const auto *value = std::get_if<double>(&initial.value.data);
            value && std::isfinite(*value)) {
            binding.initial = *value;
            correct = true;
        }
    } else if (type == "bool") {
        binding.type = LoadBindingType::Boolean;
        if (const auto *value = std::get_if<bool>(&initial.value.data)) {
            binding.initial = *value;
            correct = true;
        }
    } else if (type == "color") {
        binding.type = LoadBindingType::Color;
        if (const auto *value = std::get_if<ColorValue>(&initial.value.data)) {
            const auto rgba = value->rgba;
            binding.initial = contracts::Color{
                static_cast<std::uint8_t>(rgba >> 24), static_cast<std::uint8_t>(rgba >> 16),
                static_cast<std::uint8_t>(rgba >> 8), static_cast<std::uint8_t>(rgba)};
            correct = true;
        }
    } else {
        LoadSemanticError(source, node.line, "unknown binding type: " + type);
    }
    if (!correct) {
        LoadSemanticError(source, initial.line,
                          "binding initial value has wrong type: " + binding.name);
    }
    return binding;
}

LoadUnit ParseUnit(const SyntaxNode &node, const ComponentSource &source)
{
    RequireLeaf(node, source);
    Fields fields(node, source, {"id", "source", "phase", "after"});
    LoadUnit unit;
    unit.id = fields.Text("id");
    unit.source_path = LoadRelativePath(fields.Text("source"), source, node.line);
    unit.line = node.line;
    if (!IsLoadIdentifier(unit.id)) {
        LoadSemanticError(source, node.line, "invalid component ID: " + unit.id);
    }
    const auto phase = fields.Text("phase");
    if (phase == "critical") {
        unit.phase = LoadPhase::Critical;
    } else if (phase == "deferred") {
        unit.phase = LoadPhase::Deferred;
    } else {
        LoadSemanticError(source, node.line, "unknown component phase: " + phase);
    }
    if (const auto *after = fields.Optional("after")) {
        const auto *list = std::get_if<SyntaxValue::List>(&after->value.data);
        if (!list) {
            LoadSemanticError(source, after->line, "after requires a list of component IDs");
        }
        std::unordered_set<std::string> seen;
        for (const auto &item : *list) {
            const auto *name = std::get_if<std::string>(&item.data);
            if (!name || !IsLoadIdentifier(*name) || *name == unit.id ||
                !seen.insert(*name).second) {
                LoadSemanticError(source, after->line, "invalid, duplicate or self dependency");
            }
            unit.after.push_back(*name);
        }
    }
    return unit;
}

class GraphValidator {
public:
    explicit GraphValidator(const LoadPlan &plan) : plan_(plan)
    {
    }

    void Validate()
    {
        for (const auto &unit : plan_.components) {
            Visit(unit);
        }
    }

private:
    void Visit(const LoadUnit &unit)
    {
        const auto state = states_[unit.id];
        if (state == 2) {
            return;
        }
        if (state == 1) {
            LoadSemanticError(plan_.source, unit.line, "component dependency cycle: " + unit.id);
        }
        states_[unit.id] = 1;
        for (const auto &id : unit.after) {
            const auto *dependency = FindLoadUnit(plan_, id);
            if (!dependency) {
                LoadSemanticError(plan_.source, unit.line, "unknown component dependency: " + id);
            }
            if (unit.phase == LoadPhase::Critical && dependency->phase == LoadPhase::Deferred) {
                LoadSemanticError(plan_.source, unit.line,
                                  "critical component depends on deferred: " + id);
            }
            Visit(*dependency);
        }
        states_[unit.id] = 2;
    }

    const LoadPlan &plan_;
    std::unordered_map<std::string, unsigned> states_;
};

bool BindingMatches(const LoadBinding &binding, StoredValueType type)
{
    switch (binding.type) {
    case LoadBindingType::String:
        return type == StoredValueType::String;
    case LoadBindingType::Number:
        return type == StoredValueType::Number;
    case LoadBindingType::Boolean:
        return type == StoredValueType::Boolean;
    case LoadBindingType::Color:
        return type == StoredValueType::Color;
    }
    return false;
}
} // namespace

const LoadUnit *FindLoadUnit(const LoadPlan &plan, std::string_view id)
{
    const auto found = std::find_if(plan.components.begin(), plan.components.end(),
                                    [id](const LoadUnit &unit) { return unit.id == id; });
    return found == plan.components.end() ? nullptr : &*found;
}

bool IsInterfaceSource(std::string_view text, ComponentSource metadata)
{
    if (text.size() > kMaxLoadFileBytes) {
        LoadSemanticError(metadata, 0, "DSL source exceeds 1 MiB limit");
    }
    try {
        return compiler::Lexer(std::string(text), true).ReadRootIdentifier().text == "Interface";
    } catch (const compiler::CompilerError &error) {
        throw LoadFailure({LoadStage::Syntax, metadata, error.Line(), error.Message()});
    }
}

LoadPlan CompileLegacyLoadPlan(const PreparedComponent &prepared, ComponentSource metadata,
                               std::filesystem::path package_root)
{
    if (!prepared || prepared.SourceBytes() > kMaxLoadFileBytes) {
        LoadSemanticError(metadata, 0,
                          "legacy plan requires a valid prepared component within 1 MiB");
    }
    if (package_root.empty()) {
        LoadSemanticError(metadata, prepared.Root().line, "package root is required");
    }
    LoadPlan plan;
    plan.source = std::move(metadata);
    plan.package_root = package_root.lexically_normal();
    plan.source_bytes = prepared.SourceBytes();
    auto path = std::filesystem::path(plan.source.source_path);
    if (path.is_absolute()) {
        path = path.lexically_relative(plan.package_root);
    }
    const auto line = prepared.Root().line;
    plan.components.push_back({"master",
                               LoadRelativePath(path.generic_string(), plan.source, line),
                               LoadPhase::Critical,
                               {},
                               line});
    return plan;
}

LoadPlan CompileLoadPlan(std::string_view text, ComponentSource metadata,
                         std::filesystem::path package_root)
{
    const auto syntax = ReadLoadSyntax(text, metadata);
    if (package_root.empty()) {
        LoadSemanticError(metadata, syntax.line, "package root is required");
    }
    if (syntax.name != "Interface") {
        const auto prepared = PrepareVisualSyntax(syntax, metadata, text.size());
        return CompileLegacyLoadPlan(prepared, std::move(metadata), std::move(package_root));
    }
    LoadPlan plan;
    plan.source = std::move(metadata);
    plan.package_root = package_root.lexically_normal();
    plan.source_bytes = text.size();
    Fields fields(syntax, plan.source, {"version", "layout"});
    const auto &version = fields.Get("version");
    const auto *number = std::get_if<double>(&version.value.data);
    if (!number || *number != 2) {
        LoadSemanticError(plan.source, version.line, "Interface requires version 2");
    }
    plan.version = 2;
    plan.legacy = false;
    plan.layout_path = LoadRelativePath(fields.Text("layout"), plan.source, syntax.line);
    std::unordered_set<std::string> units, bindings;
    for (const auto &node : syntax.children) {
        if (node.name == "Binding") {
            auto binding = ParseBinding(node, plan.source);
            if (!bindings.insert(binding.name).second) {
                LoadSemanticError(plan.source, node.line,
                                  "duplicate binding declaration: " + binding.name);
            }
            plan.bindings.push_back(std::move(binding));
        } else if (node.name == "Component") {
            auto unit = ParseUnit(node, plan.source);
            if (!units.insert(unit.id).second) {
                LoadSemanticError(plan.source, node.line, "duplicate component ID: " + unit.id);
            }
            if (plan.components.size() == kMaxLoadComponents) {
                LoadSemanticError(plan.source, node.line, "component count exceeds limit of 128");
            }
            plan.components.push_back(std::move(unit));
        } else {
            LoadSemanticError(plan.source, node.line,
                              "unsupported Interface declaration: " + node.name);
        }
    }
    if (plan.components.empty()) {
        LoadSemanticError(plan.source, syntax.line, "Interface requires at least one component");
    }
    GraphValidator(plan).Validate();
    return plan;
}

void ValidateUnitBindings(const LoadPlan &plan, const PreparedNode &node,
                          const ComponentSource &source)
{
    if (!plan.legacy) {
        for (const auto &use : node.bindings) {
            const auto declaration = std::find_if(
                plan.bindings.begin(), plan.bindings.end(),
                [&use](const LoadBinding &binding) { return binding.name == use.name; });
            if (declaration == plan.bindings.end()) {
                LoadSemanticError(source, node.line, "undeclared binding: " + use.name);
            }
            const auto *spec = FindProperty(use.target);
            if (!spec || !BindingMatches(*declaration, spec->stored_type) ||
                !ValidPropertyValue(use.target, declaration->initial)) {
                LoadSemanticError(source, node.line,
                                  "binding type/initial is incompatible with target: " + use.name);
            }
        }
    }
    for (const auto &child : node.children) {
        ValidateUnitBindings(plan, child, source);
    }
}

void ValidatePreparedUnit(const LoadPlan &plan, std::string_view id,
                          const PreparedComponent &prepared)
{
    const auto *unit = FindLoadUnit(plan, id);
    if (!unit || !prepared) {
        LoadSemanticError(plan.source, unit ? unit->line : 0,
                          "unknown or invalid prepared unit: " + std::string(id));
    }
    if (!prepared.Source().component_id.empty() && prepared.Source().component_id != id) {
        LoadSemanticError(prepared.Source(), 0, "prepared component identity does not match unit");
    }
    if (prepared.SourceBytes() > kMaxLoadFileBytes) {
        LoadSemanticError(prepared.Source(), 0, "prepared unit source exceeds 1 MiB limit");
    }
    ValidateUnitBindings(plan, prepared.Root(), prepared.Source());
}

} // namespace prism::runtime
