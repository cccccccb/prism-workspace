#pragma once
#include "prism/runtime/property.hpp"
#include "prism/contracts/theme.hpp"
#include <optional>
#include <string_view>

namespace prism::runtime {
// Accepts a token name without its DSL '@' prefix. Unknown tokens are rejected.
std::optional<PropertyValue> ResolveThemeToken(const contracts::ThemeSnapshot&, std::string_view name);
} // namespace prism::runtime
