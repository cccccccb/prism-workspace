#pragma once

#include "prism/runtime/dsl_syntax.hpp"
#include "prism/runtime/prepared_component.hpp"

namespace prism::runtime {
// Owning semantic preparation metadata, without live identities or resource handles.
contracts::Contour PrepareDslContour(const SyntaxNode &, const ComponentSource &);
bool IsDslContourRecipe(const SyntaxNode &);
AttachedPanelRecipe PrepareDslContourRecipe(const SyntaxNode &, const ComponentSource &);
} // namespace prism::runtime
