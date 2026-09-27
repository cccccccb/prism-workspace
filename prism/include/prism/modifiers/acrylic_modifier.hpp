#pragma once

#include "prism/core/types.hpp"
#include "prism/modifiers/modifier.hpp"

namespace prism::modifiers {

class AcrylicModifier : public VisualModifier {
public:
    explicit AcrylicModifier(float blur_radius = 28.0f, int passes = 4,
                             core::Color tint = {22, 24, 31, 204})
        : blur_radius_(blur_radius), passes_(passes), tint_(tint)
    {
    }

    ModifierType GetType() const override
    {
        return ModifierType::Acrylic;
    }

    std::string Describe() const override
    {
        return "Acrylic(blur=" + std::to_string(blur_radius_) +
               "px, passes=" + std::to_string(passes_) + ")";
    }

    float GetBlurRadius() const
    {
        return blur_radius_;
    }

    int GetPasses() const
    {
        return passes_;
    }

    core::Color GetTint() const
    {
        return tint_;
    }

private:
    float blur_radius_{28.0f};
    int passes_{4};
    core::Color tint_{22, 24, 31, 204};
};

} // namespace prism::modifiers
