#pragma once
#include "prism/runtime/scene.hpp"
#include <string_view>

namespace prism::runtime {
// Parse once, then discard the syntax tree. Unsupported widgets fail explicitly.
Blueprint ParseBlueprint(std::string_view source);
} // namespace prism::runtime
