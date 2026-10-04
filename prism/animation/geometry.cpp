#include "prism/animation/geometry.hpp"
#include <cmath>
#include <stdexcept>

namespace prism::animation {
namespace {
bool Valid(contracts::LogicalRect r)
{
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) &&
           std::isfinite(r.height) && std::isfinite(r.x + r.width) &&
           std::isfinite(r.y + r.height) && r.width > 0 && r.height > 0;
}
} // namespace

GeometryTimeline::GeometryTimeline(const AnimationClock &clock) noexcept : progress_(clock)
{
}

bool GeometryTimeline::Reset(contracts::LogicalRect bounds)
{
    if (!Valid(bounds)) {
        return false;
    }
    // A teleport also supersedes old samples, even when no motion is running.
    const bool accepted = progress_.State() == MotionState::Idle
                              ? progress_.StartDuration(1, 1, {})
                              : progress_.RestartDuration(1, 1, {}).accepted;
    if (!accepted) {
        return false;
    }
    progress_.Sample();

    from_ = target_ = bounds;
    initialized_ = true;
    return true;
}

contracts::LogicalRect GeometryTimeline::Evaluate(double progress) const noexcept
{
    if (progress >= 1) {
        return target_;
    }
    if (progress <= 0) {
        return from_;
    }
    const auto left = std::lerp(from_.x, target_.x, progress);
    const auto top = std::lerp(from_.y, target_.y, progress);
    const auto right = std::lerp(from_.x + from_.width, target_.x + target_.width, progress);
    const auto bottom = std::lerp(from_.y + from_.height, target_.y + target_.height, progress);
    return {left, top, right - left, bottom - top};
}

bool GeometryTimeline::Retarget(contracts::LogicalRect target, DurationSpec spec,
                                MonotonicTimeNs now)
{
    if (!initialized_ || !Valid(target)) {
        return false;
    }
    if (target == target_) {
        return true;
    }
    const auto current = Running() ? Evaluate(progress_.PreviewAt(now).value) : target_;
    if (!progress_.RestartDurationAt(0, 1, spec, now).accepted) {
        return false;
    }
    from_ = current;
    target_ = target;
    return true;
}

GeometrySample GeometryTimeline::Sample(MonotonicTimeNs now)
{
    if (!initialized_) {
        throw std::logic_error("GeometryTimeline requires an initial rectangle");
    }
    const auto sample = progress_.SampleAt(now);
    return {sample.state == MotionState::Idle || sample.state == MotionState::Cancelled
                ? target_
                : Evaluate(sample.value),
            progress_.Generation(), sample.state};
}

contracts::LogicalRect GeometryTimeline::Target() const noexcept
{
    return target_;
}

bool GeometryTimeline::Running() const noexcept
{
    return progress_.State() == MotionState::Running;
}

contracts::LogicalPoint GeometryTimeline::ToSurface(const GeometrySample &submitted,
                                                    contracts::LogicalRect committed,
                                                    contracts::LogicalPoint point)
{
    if (!Valid(submitted.bounds) || !Valid(committed) || !std::isfinite(point.x) ||
        !std::isfinite(point.y)) {
        throw std::invalid_argument("Invalid presentation mapping");
    }
    return {committed.x + (point.x - submitted.bounds.x) * committed.width / submitted.bounds.width,
            committed.y +
                (point.y - submitted.bounds.y) * committed.height / submitted.bounds.height};
}
} // namespace prism::animation
