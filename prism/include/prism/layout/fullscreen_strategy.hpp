#pragma once

#include "prism/layout/layout_strategy.hpp"

namespace prism::layout {

class FluidFullscreenStrategy : public LayoutStrategy {
public:
    FluidFullscreenStrategy() = default;

    std::string GetStrategyName() const override { return "FluidFullscreen"; }

    void CalculateLayout(core::Rect screen, core::Rect& win_a, core::Rect& win_b) override {
        win_a = screen;
        win_b = core::Rect{0, 0, 0, 0}; // Hidden
    }

    void StepPhysics(float dt) override {
        // Fullscreen physics step
    }
};

} // namespace prism::layout
