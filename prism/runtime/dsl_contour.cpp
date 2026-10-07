#include "dsl_contour_p.hpp"
#include "prism/contracts/contour.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace prism::runtime {
namespace {
[[noreturn]] void Error(const ComponentSource &source, int line, std::string message)
{
    throw LoadFailure({LoadStage::Semantic, source, line, std::move(message)});
}

class Fields {
public:
    Fields(const SyntaxNode &node, const ComponentSource &source,
           std::initializer_list<std::string_view> allowed)
        : node_(node), source_(source)
    {
        if (!node.modifiers.empty()) {
            Error(source, node.modifiers.front().line,
                  node.name + " geometry declarations do not accept modifiers");
        }
        for (const auto &argument : node.arguments) {
            if (argument.name.empty() ||
                std::find(allowed.begin(), allowed.end(), argument.name) == allowed.end()) {
                Error(source, argument.line,
                      "unknown or positional " + node.name + " field: " + argument.name);
            }
            if (!fields_.emplace(argument.name, &argument).second) {
                Error(source, argument.line, "duplicate " + node.name + " field: " + argument.name);
            }
        }
    }

    const SyntaxArgument *Optional(std::string_view name) const
    {
        const auto found = fields_.find(std::string(name));
        return found == fields_.end() ? nullptr : found->second;
    }

    double Coordinate(std::string_view name) const
    {
        const auto *field = Optional(name);
        if (!field) {
            Error(source_, node_.line,
                  "missing " + node_.name + " coordinate: " + std::string(name));
        }
        const auto *value = std::get_if<double>(&field->value.data);
        if (!value || !std::isfinite(*value) ||
            std::abs(*value) > contracts::ContourCoordinateLimit) {
            Error(source_, field->line,
                  node_.name + " coordinate requires a finite numeric literal within +/-8192: " +
                      std::string(name));
        }
        return *value;
    }

    contracts::LogicalPoint Point(std::string_view x = "x", std::string_view y = "y") const
    {
        return {Coordinate(x), Coordinate(y)};
    }

private:
    const SyntaxNode &node_;
    const ComponentSource &source_;
    std::unordered_map<std::string, const SyntaxArgument *> fields_;
};

void RequireLeaf(const SyntaxNode &node, const ComponentSource &source)
{
    if (!node.children.empty()) {
        Error(source, node.children.front().line, node.name + " cannot have children");
    }
}

contracts::LogicalPoint Move(const SyntaxNode &node, const ComponentSource &source)
{
    RequireLeaf(node, source);
    return Fields(node, source, {"x", "y"}).Point();
}

void Append(contracts::ContourPath &path, const SyntaxNode &node, const ComponentSource &source)
{
    RequireLeaf(node, source);
    if (node.name == "Move") {
        Error(source, node.line, "Contour requires exactly one initial Move");
    }
    if (node.name == "Line") {
        path.segments.push_back(contracts::ContourLine{Fields(node, source, {"x", "y"}).Point()});
        return;
    }
    if (node.name == "Cubic") {
        const Fields fields(node, source, {"c1x", "c1y", "c2x", "c2y", "x", "y"});
        path.segments.push_back(contracts::ContourCubic{
            fields.Point("c1x", "c1y"), fields.Point("c2x", "c2y"), fields.Point()});
        return;
    }
    Error(source, node.line, "unsupported Contour command: " + node.name);
}
} // namespace

contracts::Contour PrepareDslContour(const SyntaxNode &node, const ComponentSource &source)
{
    const Fields fields(node, source, {"space"});
    if (const auto *field = fields.Optional("space")) {
        const auto *space = std::get_if<std::string>(&field->value.data);
        if (!space || *space != "local") {
            Error(source, field->line,
                  "Contour space must be the literal local; normalized is unsupported");
        }
    }
    if (node.children.empty()) {
        Error(source, node.line, "Contour requires an initial Move and path segments");
    }
    if (node.children.front().name != "Move") {
        Error(source, node.children.front().line, "Contour must start with Move");
    }
    if (node.children.size() - 1 > contracts::ContourVertexLimit) {
        Error(source, node.line, "Contour path segment count exceeds 256");
    }

    contracts::ContourPath path;
    path.start = Move(node.children.front(), source);
    path.segments.reserve(node.children.size() - 1);
    for (std::size_t index = 1; index < node.children.size(); ++index) {
        Append(path, node.children[index], source);
    }

    try {
        return contracts::PrepareContour(path);
    } catch (const std::invalid_argument &error) {
        Error(source, node.line, "Invalid Contour geometry: " + std::string(error.what()));
    }
}
} // namespace prism::runtime
