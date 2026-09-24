#pragma once

#include <string>
#include <memory>

namespace prism::modifiers {

enum class ModifierType {
    Blur,
    CornerRadius,
    Shadow,
    Padding,
    SpringAnimation
};

/**
 * @brief Decorator Pattern: Base interface for visual and behavioral modifiers
 */
class VisualModifier {
public:
    virtual ~VisualModifier() = default;
    virtual ModifierType GetType() const = 0;
    virtual std::string Describe() const = 0;
};

} // namespace prism::modifiers
