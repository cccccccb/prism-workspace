#pragma once

#include "prism/layout/layout_strategy.hpp"
#include <memory>
#include <vector>

namespace prism::layout {

/**
 * @brief macOS Mission Control Overview Strategy (Strategy Pattern & Decorator Pattern)
 *        Fluidly morphs between normal workspace tiling and an organized, scaled overview grid
 *        using damped harmonic spring physics.
 */
class MissionControlStrategy : public LayoutStrategy {
public:
    explicit MissionControlStrategy(std::unique_ptr<LayoutStrategy> base_strategy);
    ~MissionControlStrategy() override = default;

    std::string GetStrategyName() const override { return "MacMissionControl"; }

    void SetOverview(bool enabled);
    void ToggleOverview();
    bool IsOverviewActive() const { return target_progress_ > 0.5f; }
    float GetProgress() const { return progress_; }

    void CalculateLayout(core::Rect screen_bounds, core::Rect& win_a, core::Rect& win_b) override;
    void CalculateMultiLayout(core::Rect screen_bounds, size_t count, std::vector<core::Rect>& out_bounds) override;
    void StepPhysics(float dt) override;

    int HitTestWindow(float x, float y, const std::vector<core::Rect>& bounds) const;

    LayoutStrategy* GetBaseStrategy() const { return base_strategy_.get(); }
    void SetBaseStrategy(std::unique_ptr<LayoutStrategy> strategy) { base_strategy_ = std::move(strategy); }

private:
    std::unique_ptr<LayoutStrategy> base_strategy_;
    float progress_{0.0f};        // 0.0 = Base Layout (Split/Full), 1.0 = Mission Control Overview
    float target_progress_{0.0f};
    float velocity_{0.0f};
    float stiffness_{240.0f};     // Mac-tuned fast-response spring
    float damping_{18.0f};        // Critical damping for zero overshoot
};

} // namespace prism::layout
