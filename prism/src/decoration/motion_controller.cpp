#include "prism/decoration/motion_controller.hpp"

namespace prism::decoration {

float CubicBezierEvaluator::SampleCurveX(float x1, float x2, float t)
{
    return ((1.0f - 3.0f * x2 + 3.0f * x1) * t + (3.0f * x2 - 6.0f * x1)) * t * t + 3.0f * x1 * t;
}

float CubicBezierEvaluator::SampleCurveY(float y1, float y2, float t)
{
    return ((1.0f - 3.0f * y2 + 3.0f * y1) * t + (3.0f * y2 - 6.0f * y1)) * t * t + 3.0f * y1 * t;
}

float CubicBezierEvaluator::SampleCurveDerivativeX(float x1, float x2, float t)
{
    return (3.0f * (1.0f - 3.0f * x2 + 3.0f * x1) * t + 2.0f * (3.0f * x2 - 6.0f * x1)) * t +
           3.0f * x1;
}

float CubicBezierEvaluator::Solve(float x1, float y1, float x2, float y2, float t)
{
    if (t <= 0.0f) {
        return 0.0f;
    }
    if (t >= 1.0f) {
        return 1.0f;
    }

    // Newton-Raphson 8-step convergence
    float t2 = t;
    for (int i = 0; i < 8; ++i) {
        float x2_val = SampleCurveX(x1, x2, t2) - t;
        if (std::abs(x2_val) < 1e-4f) {
            return SampleCurveY(y1, y2, t2);
        }
        float d2 = SampleCurveDerivativeX(x1, x2, t2);
        if (std::abs(d2) < 1e-6f) {
            break;
        }
        t2 -= x2_val / d2;
    }

    // Bisection fallback
    float t0 = 0.0f, t1 = 1.0f;
    t2 = t;
    for (int i = 0; i < 12; ++i) {
        float x2_val = SampleCurveX(x1, x2, t2);
        if (std::abs(x2_val - t) < 1e-4f) {
            return SampleCurveY(y1, y2, t2);
        }
        if (t > x2_val) {
            t0 = t2;
        } else {
            t1 = t2;
        }
        t2 = (t1 + t0) * 0.5f;
    }
    return SampleCurveY(y1, y2, t2);
}

MotionController::MotionController()
{
    spring_fold_.SnapTo(0.0f);
    spring_fs_.SnapTo(0.0f);
}

void MotionController::SnapToBounds(core::Rect bounds)
{
    current_bounds_ = bounds;
    target_bounds_ = bounds;

    spring_x_.SnapTo(bounds.x);
    spring_y_.SnapTo(bounds.y);
    spring_w_.SnapTo(bounds.width);
    spring_h_.SnapTo(bounds.height);

    is_animating_ = false;
}

void MotionController::AnimateToBounds(core::Rect target, const MotionCurveSpec &spec)
{
    target_bounds_ = target;

    if (spec.engine == MotionEngine::None) {
        SnapToBounds(target);
        return;
    }

    // Configure geometry springs
    spring_x_.SetTarget(target.x, spec.damping, spec.stiffness);
    spring_y_.SetTarget(target.y, spec.damping, spec.stiffness);
    spring_w_.SetTarget(target.width, spec.damping, spec.stiffness);
    spring_h_.SetTarget(target.height, spec.damping, spec.stiffness);

    is_animating_ = true;
}

void MotionController::SetFolded(bool folded, const MotionCurveSpec &spec)
{
    if (is_folded_ == folded && spring_fold_.IsSettled()) {
        return;
    }
    is_folded_ = folded;
    float target_val = folded ? 1.0f : 0.0f;

    if (spec.engine == MotionEngine::Spring) {
        fold_uses_spring_ = true;
        spring_fold_.SetTarget(target_val, spec.damping, spec.stiffness);
    } else {
        fold_uses_spring_ = false;
        float cur = fold_uses_spring_ ? spring_fold_.pos : tween_fold_.current;
        tween_fold_.Start(cur, target_val, spec.duration_ms, spec.bezier_x1, spec.bezier_y1,
                          spec.bezier_x2, spec.bezier_y2);
    }
    is_animating_ = true;
}

void MotionController::SetFullscreen(bool fullscreen, core::Rect screen_bounds,
                                     const MotionCurveSpec &spec)
{
    if (is_fullscreen_ == fullscreen && spring_fs_.IsSettled() && tween_fs_.IsSettled()) {
        return;
    }

    if (fullscreen && !is_fullscreen_) {
        pre_fullscreen_bounds_ = target_bounds_;
        AnimateToBounds(screen_bounds, spec);
    } else if (!fullscreen && is_fullscreen_) {
        AnimateToBounds(pre_fullscreen_bounds_, spec);
    }

    is_fullscreen_ = fullscreen;
    float target_val = fullscreen ? 1.0f : 0.0f;
    if (spec.engine == MotionEngine::Spring) {
        fs_uses_spring_ = true;
        spring_fs_.SetTarget(target_val, spec.damping, spec.stiffness);
    } else {
        fs_uses_spring_ = false;
        float cur = fs_uses_spring_ ? spring_fs_.pos : tween_fs_.current;
        tween_fs_.Start(cur, target_val, spec.duration_ms, spec.bezier_x1, spec.bezier_y1,
                        spec.bezier_x2, spec.bezier_y2);
    }
    is_animating_ = true;
}

void MotionController::SetFocused(bool focused, const MotionCurveSpec &spec)
{
    is_focused_ = focused;
    float target_val = focused ? 1.0f : 0.0f;
    float cur = tween_focus_.current;
    tween_focus_.Start(cur, target_val, spec.duration_ms, spec.bezier_x1, spec.bezier_y1,
                       spec.bezier_x2, spec.bezier_y2);
    is_animating_ = true;
}

bool MotionController::Step(float dt)
{
    if (!is_animating_) {
        return false;
    }

    // Step geometry springs
    spring_x_.Step(dt);
    spring_y_.Step(dt);
    spring_w_.Step(dt);
    spring_h_.Step(dt);

    current_bounds_.x = spring_x_.pos;
    current_bounds_.y = spring_y_.pos;
    current_bounds_.width = spring_w_.pos;
    current_bounds_.height = spring_h_.pos;

    // Step fold
    if (fold_uses_spring_) {
        spring_fold_.Step(dt);
    } else {
        tween_fold_.Step(dt);
    }

    // Step fullscreen
    if (fs_uses_spring_) {
        spring_fs_.Step(dt);
    } else {
        tween_fs_.Step(dt);
    }

    // Step focus
    tween_focus_.Step(dt);

    // Check if settled
    bool geom_settled = spring_x_.IsSettled() && spring_y_.IsSettled() && spring_w_.IsSettled() &&
                        spring_h_.IsSettled();
    bool fold_settled = fold_uses_spring_ ? spring_fold_.IsSettled() : tween_fold_.IsSettled();
    bool fs_settled = fs_uses_spring_ ? spring_fs_.IsSettled() : tween_fs_.IsSettled();
    bool focus_settled = tween_focus_.IsSettled();

    if (geom_settled && fold_settled && fs_settled && focus_settled) {
        is_animating_ = false;
    }

    return is_animating_;
}

core::Rect MotionController::GetVisualBounds(float header_height, float border_width) const
{
    core::Rect b = current_bounds_;
    float fold_r = GetFoldRatio();
    if (fold_r > 0.001f) {
        float min_h = header_height + 2.0f * border_width;
        b.height = b.height + (min_h - b.height) * fold_r;
    }
    return b;
}

float MotionController::GetFoldRatio() const
{
    float r = fold_uses_spring_ ? spring_fold_.pos : tween_fold_.current;
    return std::clamp(r, 0.0f, 1.0f);
}

float MotionController::GetFullscreenRatio() const
{
    float r = fs_uses_spring_ ? spring_fs_.pos : tween_fs_.current;
    return std::clamp(r, 0.0f, 1.0f);
}

float MotionController::GetFocusRatio() const
{
    return std::clamp(tween_focus_.current, 0.0f, 1.0f);
}

float MotionController::GetContentAlpha() const
{
    // When folding, content fades out smoothly
    float fold_r = GetFoldRatio();
    return std::clamp(1.0f - fold_r, 0.0f, 1.0f);
}

} // namespace prism::decoration
