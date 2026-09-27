#pragma once

#include "prism/modifiers/modifier.hpp"

namespace prism::modifiers {

class KawaseBlurModifier : public VisualModifier {
public:
    explicit KawaseBlurModifier(float radius = 30.0f, int passes = 4)
        : radius_(radius), passes_(passes)
    {
    }

    ModifierType GetType() const override
    {
        return ModifierType::Blur;
    }

    std::string Describe() const override
    {
        return "KawaseBlur(radius=" + std::to_string(radius_) +
               "px, passes=" + std::to_string(passes_) + ")";
    }

    float GetRadius() const
    {
        return radius_;
    }

    int GetPasses() const
    {
        return passes_;
    }

private:
    float radius_{30.0f};
    int passes_{4};
};

} // namespace prism::modifiers
