#include "prism/runtime/theme_tokens.hpp"

namespace prism::runtime {
std::optional<PropertyValue> ResolveThemeToken(const contracts::ThemeSnapshot& theme, std::string_view name) {
    for (const auto& token : theme.numbers)
        if (token.name == name) return token.value;
    for (const auto& token : theme.colors)
        if (token.name == name) return token.value;
    return std::nullopt;
}
} // namespace prism::runtime
