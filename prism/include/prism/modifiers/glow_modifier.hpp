#pragma once

#include "prism/core/types.hpp"
#include "prism/modifiers/modifier.hpp"

namespace prism::modifiers {

class GlowModifier : public VisualModifier {
public:
    explicit GlowModifier(float radius = 16.0f, core::Color color = {56, 239, 125, 80})
        : radius_(radius), color_(color)
    {
    }

    ModifierType GetType() const override
    {
        return ModifierType::Glow;
    }

    std::string Describe() const override
    {
        return "Glow(radius=" + std::to_string(radius_) + "px)";
    }

    float GetRadius() const
    {
        return radius_;
    }

    core::Color GetColor() const
    {
        return color_;
    }

private:
    float radius_{16.0f};
    core::Color color_{56, 239, 125, 80};
};

} // namespace prism::modifiers
