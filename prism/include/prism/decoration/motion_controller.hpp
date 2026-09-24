#pragma once

#include "prism/core/types.hpp"
#include "prism/decoration/tiling_decoration_spec.hpp"
#include <algorithm>
#include <cmath>

namespace prism::decoration {

enum class MotionType {
    None,
    SplitMove,
    Fold,
    Fullscreen,
    Focus
};

/**
 * @brief High-precision Damped Harmonic Oscillator (Spring Physics)
 * 
 * Differential equation: m*x'' + c*x' + k*(x - target) = 0
 * With unit mass m=1, damping coefficient c = 2 * damping * sqrt(stiffness).
 * Uses clamped sub-stepping for unconditional stability regardless of frame time jitter.
 */
struct SpringSolver {
    float pos{0.0f};
    float target{0.0f};
    float vel{0.0f};
    float damping{0.80f};
    float stiffness{220.0f};

    void SnapTo(float val) {
        pos = val;
        target = val;
        vel = 0.0f;
    }

    void SetTarget(float new_target, float new_damping, float new_stiffness) {
        target = new_target;
        damping = new_damping;
        stiffness = new_stiffness;
    }

    void Step(float dt) {
        if (IsSettled()) return;

        // Sub-stepping: dt_sub <= 1/240s to guarantee numerical stability
        const float sub_dt = 1.0f / 240.0f;
        int steps = std::max(1, static_cast<int>(std::ceil(dt / sub_dt)));
        steps = std::min(steps, 8); // clamp maximum steps to prevent spiral
        const float actual_sub_dt = dt / static_cast<float>(steps);
        const float damping_coeff = 2.0f * damping * std::sqrt(std::max(1.0f, stiffness));

        for (int i = 0; i < steps; ++i) {
            float force = -stiffness * (pos - target) - damping_coeff * vel;
            vel += force * actual_sub_dt;
            pos += vel * actual_sub_dt;
        }

        // Settling threshold
        if (std::abs(pos - target) < 0.05f && std::abs(vel) < 0.1f) {
            pos = target;
            vel = 0.0f;
        }
    }

    bool IsSettled() const {
        return pos == target && vel == 0.0f;
    }
};

/**
 * @brief Cubic Bézier Curve Timing Evaluator
 * 
 * Solves B_x(u) = t using Newton-Raphson iteration, then evaluates B_y(u).
 */
class CubicBezierEvaluator {
public:
    static float Solve(float x1, float y1, float x2, float y2, float t);

private:
    static float SampleCurveX(float x1, float x2, float t);
    static float SampleCurveY(float y1, float y2, float t);
    static float SampleCurveDerivativeX(float x1, float x2, float t);
};

/**
 * @brief Parametric Tween State for Bezier / Linear transitions
 */
struct TweenSolver {
    float start{0.0f};
    float target{0.0f};
    float current{0.0f};
    float elapsed_ms{0.0f};
    float duration_ms{250.0f};
    float x1{0.25f};
    float y1{0.10f};
    float x2{0.25f};
    float y2{1.00f};
    bool active{false};

    void Start(float from, float to, float duration, float bx1, float by1, float bx2, float by2) {
        start = from;
        current = from;
        target = to;
        duration_ms = std::max(1.0f, duration);
        elapsed_ms = 0.0f;
        x1 = bx1;
        y1 = by1;
        x2 = bx2;
        y2 = by2;
        active = true;
    }

    void Step(float dt) {
        if (!active) return;
        elapsed_ms += dt * 1000.0f;
        float progress = std::clamp(elapsed_ms / duration_ms, 0.0f, 1.0f);
        float eased = CubicBezierEvaluator::Solve(x1, y1, x2, y2, progress);
        current = start + (target - start) * eased;

        if (progress >= 1.0f) {
            current = target;
            active = false;
        }
    }

    bool IsSettled() const {
        return !active;
    }
};

/**
 * @brief Window Motion Controller
 * 
 * Orchestrates multi-channel kinetic animations for a window decorator:
 * - 4-axis Geometry Springs (Split / Tile Slide / Reorder)
 * - Fold & Roll-up Spring/Tween (Collapse to Titlebar)
 * - Monocle / Fullscreen Tween (Expand to Screen & Restore)
 * - Focus Pulse Interpolation
 */
class MotionController {
public:
    MotionController();

    void SnapToBounds(core::Rect bounds);
    void AnimateToBounds(core::Rect target, const MotionCurveSpec& spec);
    void SetFolded(bool folded, const MotionCurveSpec& spec);
    void SetFullscreen(bool fullscreen, core::Rect screen_bounds, const MotionCurveSpec& spec);
    void SetFocused(bool focused, const MotionCurveSpec& spec);

    // Advances physics by dt (seconds). Returns true if animations are still running.
    bool Step(float dt);

    // Visual State Queries
    core::Rect GetVisualBounds(float header_height, float border_width) const;
    core::Rect GetTargetBounds() const { return target_bounds_; }
    float GetFoldRatio() const;
    float GetFullscreenRatio() const;
    float GetFocusRatio() const;
    float GetContentAlpha() const;

    bool IsFolded() const { return is_folded_; }
    bool IsFullscreen() const { return is_fullscreen_; }
    bool IsFocused() const { return is_focused_; }
    bool IsAnimating() const { return is_animating_; }

private:
    core::Rect current_bounds_{0, 0, 0, 0};
    core::Rect target_bounds_{0, 0, 0, 0};
    core::Rect pre_fullscreen_bounds_{0, 0, 0, 0};

    // Geometry Springs
    SpringSolver spring_x_;
    SpringSolver spring_y_;
    SpringSolver spring_w_;
    SpringSolver spring_h_;

    // Fold Channel (0.0 = fully open, 1.0 = fully folded to titlebar)
    SpringSolver spring_fold_;
    TweenSolver  tween_fold_;
    bool         fold_uses_spring_{true};
    bool         is_folded_{false};

    // Fullscreen Channel (0.0 = tiled, 1.0 = fullscreen)
    SpringSolver spring_fs_;
    TweenSolver  tween_fs_;
    bool         fs_uses_spring_{false};
    bool         is_fullscreen_{false};

    // Focus Channel (0.0 = unfocused, 1.0 = focused)
    TweenSolver  tween_focus_;
    bool         is_focused_{false};

    bool is_animating_{false};
};

} // namespace prism::decoration
