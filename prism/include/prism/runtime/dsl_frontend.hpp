#pragma once
#include "prism/runtime/scene.hpp"
#include <functional>
#include <string_view>

namespace prism::runtime {
// Parse once, then discard the syntax tree. Unsupported widgets fail explicitly.
using ResolveImage = std::function<contracts::ResourceId(std::string_view)>;
Blueprint ParseBlueprint(std::string_view source, ResolveImage resolve_image = {});
} // namespace prism::runtime
