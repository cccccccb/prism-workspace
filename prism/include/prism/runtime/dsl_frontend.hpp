#pragma once
#include "prism/runtime/prepared_component.hpp"
#include <string_view>

namespace prism::runtime {
// Compatibility entry point; always completes PrepareComponent before resource linking.
Blueprint ParseBlueprint(std::string_view source, ResolveImage resolve_image = {});
} // namespace prism::runtime
