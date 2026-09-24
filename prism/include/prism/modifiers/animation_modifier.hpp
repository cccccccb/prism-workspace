#pragma once

#include "prism/modifiers/modifier.hpp"

namespace prism::modifiers {

class SpringAnimationModifier : public VisualModifier {
public:
    SpringAnimationModifier(float damping = 0.75f, float stiffness = 150.0f, float initial_velocity = 0.0f)
        : damping_(damping), stiffness_(stiffness), velocity_(initial_velocity) {}

    ModifierType GetType() const override { return ModifierType::SpringAnimation; }

    std::string Describe() const override {
        return "SpringAnimation(damping=" + std::to_string(damping_) + ", stiffness=" + std::to_string(stiffness_) + ")";
    }

    float GetDamping() const { return damping_; }
    float GetStiffness() const { return stiffness_; }
    float GetVelocity() const { return velocity_; }

private:
    float damping_{0.75f};
    float stiffness_{150.0f};
    float velocity_{0.0f};
};

} // namespace prism::modifiers
