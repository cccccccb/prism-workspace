#pragma once

#include "prism/modifiers/modifier.hpp"

namespace prism::modifiers {

class HoverSpringModifier : public VisualModifier {
public:
    explicit HoverSpringModifier(float scale = 1.03f, float damping = 0.82f)
        : scale_(scale), damping_(damping) {}

    ModifierType GetType() const override { return ModifierType::HoverSpring; }

    std::string Describe() const override {
        return "HoverSpring(scale=" + std::to_string(scale_) + ", damping=" + std::to_string(damping_) + ")";
    }

    float GetScale() const { return scale_; }
    float GetDamping() const { return damping_; }

private:
    float scale_{1.03f};
    float damping_{0.82f};
};

} // namespace prism::modifiers
