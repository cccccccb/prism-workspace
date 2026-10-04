#pragma once
#include "prism/animation/timeline.hpp"
#include "prism/contracts/types.hpp"

namespace prism::animation {
struct GeometrySample {
    contracts::LogicalRect bounds;
    std::uint64_t generation{};
    MotionState state{MotionState::Idle};
};

// Owner-thread adapter. It never mutates a layout tree or configures a client.
// Input consumers retain the last successfully submitted sample, not Sample(now).
class GeometryTimeline {
public:
    explicit GeometryTimeline(const AnimationClock &clock) noexcept;
    bool Reset(contracts::LogicalRect bounds);
    bool Retarget(contracts::LogicalRect target, DurationSpec spec, MonotonicTimeNs now);
    GeometrySample Sample(MonotonicTimeNs now);
    contracts::LogicalRect Target() const noexcept;
    bool Running() const noexcept;
    static contracts::LogicalPoint ToSurface(const GeometrySample &submitted,
                                             contracts::LogicalRect committed,
                                             contracts::LogicalPoint point);

private:
    contracts::LogicalRect Evaluate(double progress) const noexcept;
    ScalarTimeline progress_;
    contracts::LogicalRect from_, target_;
    bool initialized_{};
};
} // namespace prism::animation
