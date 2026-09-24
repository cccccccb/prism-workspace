#include "prism/layout/mission_control_strategy.hpp"
#include "prism/core/logging.hpp"
#include <algorithm>
#include <cmath>

namespace prism::layout {

MissionControlStrategy::MissionControlStrategy(std::unique_ptr<LayoutStrategy> base_strategy)
    : base_strategy_(std::move(base_strategy)) {}

void MissionControlStrategy::SetOverview(bool enabled) {
    target_progress_ = enabled ? 1.0f : 0.0f;
    PRISM_LOG_INFO("LAYOUT-MC", "Mission Control overview %s (target_progress: %.1f)",
                   enabled ? "ENGAGED" : "DISENGAGED", target_progress_);
}

void MissionControlStrategy::ToggleOverview() {
    SetOverview(target_progress_ < 0.5f);
}

void MissionControlStrategy::StepPhysics(float dt) {
    if (base_strategy_) {
        base_strategy_->StepPhysics(dt);
    }

    // Step spring physics for overview transition
    while (dt > 0.0f) {
        float step = std::min(dt, 0.01f);
        float diff = target_progress_ - progress_;
        velocity_ += diff * stiffness_ * step;
        velocity_ *= std::max(0.0f, 1.0f - damping_ * step);
        progress_ += velocity_ * step;
        dt -= step;
    }

    // Stable boundary clamping
    if (std::abs(target_progress_ - progress_) < 0.001f && std::abs(velocity_) < 0.005f) {
        progress_ = target_progress_;
        velocity_ = 0.0f;
    }
}

void MissionControlStrategy::CalculateLayout(core::Rect screen_bounds, core::Rect& win_a, core::Rect& win_b) {
    std::vector<core::Rect> results;
    CalculateMultiLayout(screen_bounds, 2, results);
    if (results.size() >= 2) {
        win_a = results[0];
        win_b = results[1];
    }
}

void MissionControlStrategy::CalculateMultiLayout(core::Rect screen_bounds, size_t count, std::vector<core::Rect>& out_bounds) {
    out_bounds.clear();
    if (count == 0) return;

    // 1. Compute Base Layout (from underlying strategy, e.g. MacFluidSplit)
    std::vector<core::Rect> base_bounds;
    if (base_strategy_) {
        base_strategy_->CalculateMultiLayout(screen_bounds, count, base_bounds);
    } else {
        base_bounds.resize(count, screen_bounds);
    }

    if (progress_ <= 0.0001f) {
        out_bounds = base_bounds;
        return;
    }

    // 2. Compute Overview Target Grid Layout
    std::vector<core::Rect> overview_bounds(count);

    float top_margin = 110.0f;    // Reserved for top Spaces bar & window title tags
    float bottom_margin = 130.0f; // Reserved for macOS Floating Dock
    float side_margin = 80.0f;
    float gap = 50.0f;

    float avail_w = screen_bounds.width - side_margin * 2.0f;
    float avail_h = screen_bounds.height - top_margin - bottom_margin;

    if (count == 1) {
        float card_w = std::min(1000.0f, avail_w);
        float card_h = card_w * (screen_bounds.height / screen_bounds.width);
        float cx = screen_bounds.x + (screen_bounds.width - card_w) * 0.5f;
        float cy = screen_bounds.y + top_margin + (avail_h - card_h) * 0.5f;
        overview_bounds[0] = core::Rect{cx, cy, card_w, card_h};
    } else if (count == 2) {
        float card_w = std::min(780.0f, (avail_w - gap) * 0.5f);
        float card_h = card_w * (screen_bounds.height / screen_bounds.width);
        float total_w = card_w * 2.0f + gap;
        float start_x = screen_bounds.x + (screen_bounds.width - total_w) * 0.5f;
        float cy = screen_bounds.y + top_margin + (avail_h - card_h) * 0.5f;

        overview_bounds[0] = core::Rect{start_x, cy, card_w, card_h};
        overview_bounds[1] = core::Rect{start_x + card_w + gap, cy, card_w, card_h};
    } else {
        // Generalized grid for 3+ windows
        int cols = static_cast<int>(std::ceil(std::sqrt(count)));
        int rows = static_cast<int>(std::ceil(static_cast<float>(count) / cols));
        float card_w = (avail_w - (cols - 1) * gap) / cols;
        float card_h = (avail_h - (rows - 1) * gap) / rows;
        for (size_t i = 0; i < count; ++i) {
            int r = i / cols;
            int c = i % cols;
            overview_bounds[i] = core::Rect{
                screen_bounds.x + side_margin + c * (card_w + gap),
                screen_bounds.y + top_margin + r * (card_h + gap),
                card_w,
                card_h
            };
        }
    }

    // 3. Fluid Spring Interpolation between base bounds and overview bounds
    out_bounds.resize(count);
    float t = std::clamp(progress_, 0.0f, 1.0f);
    for (size_t i = 0; i < count; ++i) {
        const auto& b = base_bounds[i];
        const auto& o = overview_bounds[i];
        out_bounds[i] = core::Rect{
            b.x + (o.x - b.x) * t,
            b.y + (o.y - b.y) * t,
            b.width + (o.width - b.width) * t,
            b.height + (o.height - b.height) * t
        };
    }
}

int MissionControlStrategy::HitTestWindow(float x, float y, const std::vector<core::Rect>& bounds) const {
    for (int i = static_cast<int>(bounds.size()) - 1; i >= 0; --i) {
        const auto& r = bounds[i];
        if (x >= r.x && x <= r.x + r.width && y >= r.y && y <= r.y + r.height) {
            return i;
        }
    }
    return -1;
}

} // namespace prism::layout
