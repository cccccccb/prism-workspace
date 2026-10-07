#include "dsl_contour_p.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>

namespace prism::runtime {
namespace {
[[noreturn]] void Error(const ComponentSource &source, int line, std::string message)
{
    throw LoadFailure({LoadStage::Semantic, source, line, std::move(message)});
}

bool ThemeNumberName(std::string_view name)
{
    if (name.empty() || name.size() > 64) {
        return false;
    }
    for (const auto c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

class RecipeFields {
public:
    RecipeFields(const SyntaxNode &node, const ComponentSource &source)
        : node_(node), source_(source)
    {
        constexpr std::array<std::string_view, 6> allowed{"recipe",     "radius",   "neckWidth",
                                                          "neckHeight", "fallback", "neckShape"};
        for (const auto &argument : node.arguments) {
            if (argument.name.empty() ||
                std::find(allowed.begin(), allowed.end(), argument.name) == allowed.end()) {
                Error(source_, argument.line,
                      "unknown or positional Contour recipe field: " + argument.name);
            }
            if (!fields_.emplace(argument.name, &argument).second) {
                Error(source_, argument.line, "duplicate Contour recipe field: " + argument.name);
            }
        }
    }

    void Literal(std::string_view name, std::string_view expected) const
    {
        const auto &field = Required(name);
        const auto *value = std::get_if<std::string>(&field.value.data);
        if (!value || *value != expected) {
            Error(source_, field.line,
                  "Contour recipe " + std::string(name) + " requires the literal " +
                      std::string(expected));
        }
    }

    contracts::PanelNeckShape NeckShape() const
    {
        const auto found = fields_.find("neckShape");
        if (found == fields_.end()) {
            return contracts::PanelNeckShape::SoftTab;
        }
        const auto &field = *found->second;
        const auto *value = std::get_if<std::string>(&field.value.data);
        if (value && *value == "softTab") {
            return contracts::PanelNeckShape::SoftTab;
        }
        if (value && *value == "roundedTriangle") {
            return contracts::PanelNeckShape::RoundedTriangle;
        }
        Error(source_, field.line,
              "Contour recipe neckShape requires the literal softTab or roundedTriangle");
    }

    ContourNumber Number(std::string_view name, double maximum) const
    {
        const auto &field = Required(name);
        if (const auto *number = std::get_if<double>(&field.value.data)) {
            if (!std::isfinite(*number) || *number < 0 || *number > maximum) {
                Error(source_, field.line,
                      "Contour recipe " + std::string(name) +
                          " requires a finite numeric literal within 0.." +
                          std::to_string(static_cast<unsigned>(maximum)));
            }
            return *number;
        }
        if (const auto *text = std::get_if<std::string>(&field.value.data);
            text && text->starts_with('@')) {
            const std::string_view reference(*text);
            const auto token = reference.substr(1);
            if (!ThemeNumberName(token)) {
                Error(source_, field.line,
                      "Contour recipe " + std::string(name) +
                          " requires a valid @theme number name of at most 64 bytes");
            }
            return ContourThemeNumber{std::string(token)};
        }
        Error(source_, field.line,
              "Contour recipe " + std::string(name) +
                  " requires a finite numeric literal or @theme number");
    }

private:
    const SyntaxArgument &Required(std::string_view name) const
    {
        const auto found = fields_.find(std::string(name));
        if (found == fields_.end()) {
            Error(source_, node_.line, "missing Contour recipe field: " + std::string(name));
        }
        return *found->second;
    }

    const SyntaxNode &node_;
    const ComponentSource &source_;
    std::unordered_map<std::string, const SyntaxArgument *> fields_;
};
} // namespace

bool IsDslContourRecipe(const SyntaxNode &node)
{
    return std::any_of(node.arguments.begin(), node.arguments.end(),
                       [](const SyntaxArgument &argument) { return argument.name == "recipe"; });
}

AttachedPanelRecipe PrepareDslContourRecipe(const SyntaxNode &node, const ComponentSource &source)
{
    if (!node.modifiers.empty()) {
        Error(source, node.modifiers.front().line,
              "Contour recipe geometry declarations do not accept modifiers");
    }
    if (!node.children.empty()) {
        Error(source, node.children.front().line, "Contour recipe cannot have children");
    }

    const RecipeFields fields(node, source);
    fields.Literal("recipe", "attachedPanel");
    fields.Literal("fallback", "detached");

    return {fields.Number("radius", 256), fields.Number("neckWidth", 256),
            fields.Number("neckHeight", 48), ContourFallback::Detached, fields.NeckShape()};
}

} // namespace prism::runtime
