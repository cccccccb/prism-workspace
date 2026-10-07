#pragma once

#include "prepared_component_p.hpp"
#include "prism/runtime/dsl_syntax.hpp"
#include "prism/runtime/load_plan.hpp"

namespace prism::runtime {

SyntaxNode ReadLoadSyntax(std::string_view source, const ComponentSource &);
[[noreturn]] void LoadSemanticError(const ComponentSource &, int line, std::string message);
void ValidateUnitBindings(const LoadPlan &, const PreparedNode &, const ComponentSource &);
bool IsLoadIdentifier(std::string_view);
std::filesystem::path LoadRelativePath(std::string_view, const ComponentSource &, int line);

} // namespace prism::runtime
