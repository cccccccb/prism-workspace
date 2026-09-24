#pragma once

#include "prism/layout/layout_strategy.hpp"

namespace prism::layout {

class MacFluidSplitStrategy : public LayoutStrategy {
public:
    explicit MacFluidSplitStrategy(float initial_ratio = 0.5f)
        : current_ratio_(initial_ratio), target_ratio_(initial_ratio) {}

    std::string GetStrategyName() const override { return "MacFluidSplit"; }

    void SetTargetRatio(float ratio) { target_ratio_ = ratio; }

    void CalculateLayout(core::Rect screen, core::Rect& win_a, core::Rect& win_b) override {
        float width_a = screen.width * current_ratio_;
        float width_b = screen.width - width_a;

        win_a = core::Rect{screen.x, screen.y, width_a, screen.height};
        win_b = core::Rect{screen.x + width_a, screen.y, width_b, screen.height};
    }

    void StepPhysics(float dt) override {
        // Sub-step physics at 10ms intervals for rock-solid numerical stability
        while (dt > 0.0f) {
            float step = std::min(dt, 0.01f);
            float diff = target_ratio_ - current_ratio_;
            velocity_ += diff * stiffness_ * step;
            velocity_ *= std::max(0.0f, 1.0f - damping_ * step);
            current_ratio_ += velocity_ * step;
            dt -= step;
        }
    }

    float GetCurrentRatio() const { return current_ratio_; }

private:
    float current_ratio_{0.5f};
    float target_ratio_{0.5f};
    float velocity_{0.0f};
    float stiffness_{180.0f};
    float damping_{12.0f};
};

} // namespace prism::layout
