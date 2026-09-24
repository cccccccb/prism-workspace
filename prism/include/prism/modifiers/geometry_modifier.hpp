#pragma once

#include "prism/modifiers/modifier.hpp"
#include "prism/core/types.hpp"

namespace prism::modifiers {

class CornerRadiusModifier : public VisualModifier {
public:
    explicit CornerRadiusModifier(float radius = 16.0f) : radius_(radius) {}

    ModifierType GetType() const override { return ModifierType::CornerRadius; }

    std::string Describe() const override {
        return "CornerRadius(" + std::to_string(radius_) + "px)";
    }

    float GetRadius() const { return radius_; }

private:
    float radius_{16.0f};
};

class ShadowModifier : public VisualModifier {
public:
    ShadowModifier(float radius = 25.0f, float y_offset = 10.0f, core::Color color = core::Color::FromHex(0x00000080))
        : radius_(radius), y_offset_(y_offset), color_(color) {}

    ModifierType GetType() const override { return ModifierType::Shadow; }

    std::string Describe() const override {
        return "Shadow(radius=" + std::to_string(radius_) + "px, dy=" + std::to_string(y_offset_) + ")";
    }

    float GetRadius() const { return radius_; }
    float GetYOffset() const { return y_offset_; }
    core::Color GetColor() const { return color_; }

private:
    float radius_{25.0f};
    float y_offset_{10.0f};
    core::Color color_;
};

class PaddingModifier : public VisualModifier {
public:
    explicit PaddingModifier(float all = 16.0f)
        : top_(all), right_(all), bottom_(all), left_(all) {}

    PaddingModifier(float top, float right, float bottom, float left)
        : top_(top), right_(right), bottom_(bottom), left_(left) {}

    ModifierType GetType() const override { return ModifierType::Padding; }

    std::string Describe() const override {
        return "Padding(" + std::to_string(top_) + ", " + std::to_string(right_) + ")";
    }

    float GetTop() const { return top_; }
    float GetRight() const { return right_; }
    float GetBottom() const { return bottom_; }
    float GetLeft() const { return left_; }

private:
    float top_{0.0f};
    float right_{0.0f};
    float bottom_{0.0f};
    float left_{0.0f};
};

} // namespace prism::modifiers
