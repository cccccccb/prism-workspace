#include "prism/runtime/theme_tokens.hpp"
#include "prism/contracts/theme_tokens.hpp"

namespace prism::runtime {
std::optional<PropertyValue> ResolveThemeToken(std::string_view name) {
    for (const auto& token : contracts::theme::kNumbers)
        if (token.name == name) return token.value;
    for (const auto& token : contracts::theme::kColors)
        if (token.name == name) return token.value;
    return std::nullopt;
}
} // namespace prism::runtime
