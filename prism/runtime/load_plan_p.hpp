#pragma once

#include "prepared_component_p.hpp"
#include "prism/runtime/dsl_syntax.hpp"
#include "prism/runtime/load_plan.hpp"
#include <utility>

namespace prism::runtime {

// Internal access keeps the public prepared result immutable and AST-free.
struct PreparedComponentAccess {
    static PreparedComponent Make(ComponentSource source, PreparedNode root,
                                  std::vector<PreparedImage> images, std::size_t bytes,
                                  std::size_t nodes)
    {
        auto data = std::make_shared<PreparedComponent::Data>();
        data->source = std::move(source);
        data->root = std::move(root);
        data->images = std::move(images);
        data->source_bytes = bytes;
        data->node_count = nodes;
        return PreparedComponent(std::move(data));
    }
};

PreparedComponent PrepareVisualSyntax(const SyntaxNode &, ComponentSource, std::size_t bytes);
SyntaxNode ReadLoadSyntax(std::string_view source, const ComponentSource &);
[[noreturn]] void LoadSemanticError(const ComponentSource &, int line, std::string message);
void ValidateUnitBindings(const LoadPlan &, const PreparedNode &, const ComponentSource &);
bool IsLoadIdentifier(std::string_view);
std::filesystem::path LoadRelativePath(std::string_view, const ComponentSource &, int line);

} // namespace prism::runtime
