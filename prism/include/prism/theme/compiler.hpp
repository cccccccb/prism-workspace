#pragma once
#include "prism/contracts/theme.hpp"
#include <filesystem>
#include <string_view>

namespace prism::theme {
// Resolve a complete theme DSL into a backend-independent immutable value.
contracts::ThemeSnapshot CompileTheme(std::string_view source,std::uint64_t generation=0);
// Packages contain one versioned theme.prism. The ID and resources stay below root.
contracts::ThemeSnapshot LoadTheme(const std::filesystem::path& root,std::string_view id,
                                   std::uint64_t generation=0);
std::filesystem::path DefaultThemeRoot();
} // namespace prism::theme
