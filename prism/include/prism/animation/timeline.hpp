#pragma once

#include <cstdint>
#include <optional>

namespace prism::animation {

using MonotonicTimeNs = std::uint64_t;

// The clock and every deadline in this interface use CLOCK_MONOTONIC nanoseconds.
// A timeline borrows its clock; the clock must outlive the timeline.
class AnimationClock {
public:
    virtual ~AnimationClock() = default;
    virtual MonotonicTimeNs NowNs() const noexcept = 0;
};

class SystemAnimationClock final : public AnimationClock {
public:
    MonotonicTimeNs NowNs() const noexcept override;
};

enum class Easing { Linear, EaseInCubic, EaseOutCubic, EaseInOutCubic };

struct DurationSpec {
    MonotonicTimeNs duration_ns{};
    MonotonicTimeNs delay_ns{};
    Easing easing{Easing::Linear};
};

// Angular frequency is in radians per second. A spring has no nominal duration;
// max_duration_ns is a safety deadline after which it settles at the exact target.
// Frequency must be in (0, 10000], damping ratio in [0, 10], and both
// tolerances and the maximum duration must be positive.
struct SpringSpec {
    double angular_frequency_rad_per_sec{18.0};
    double damping_ratio{1.0};
    double position_epsilon{0.001};
    double velocity_epsilon{0.01};
    MonotonicTimeNs max_duration_ns{3'000'000'000};
};

enum class MotionState { Idle, Running, Paused, Finished, Cancelled };
enum class TerminalReason { Finished, Cancelled, Superseded };

struct TerminalEvent {
    std::uint64_t generation{};
    TerminalReason reason{TerminalReason::Finished};
};

struct MotionSample {
    double value{};
    double velocity{}; // Units per second.
    MotionState state{MotionState::Idle};
    std::optional<TerminalEvent> terminal;
};

struct RetargetResult {
    bool accepted{};
    std::optional<TerminalEvent> previous_terminal;
};

// Scalar trajectories are evaluated from their start time at every sample.
// Sampling frequency never changes the value at a given monotonic timestamp.
// The owner chooses when to sample and how to quantize or present the result.
class ScalarTimeline {
public:
    explicit ScalarTimeline(const AnimationClock &clock) noexcept;

    bool StartDuration(double from, double target, DurationSpec spec);
    bool StartDurationAt(double from, double target, DurationSpec spec, MonotonicTimeNs now);
    bool StartSpring(double from, double target, double initial_velocity, SpringSpec spec);
    bool StartSpringAt(double from, double target, double initial_velocity, SpringSpec spec,
                       MonotonicTimeNs now);
    // Start from an externally captured visual value, superseding an active
    // generation. Useful for a normalized 0..1 track with typed endpoints.
    RetargetResult RestartDuration(double from, double target, DurationSpec spec);
    RetargetResult RestartDurationAt(double from, double target, DurationSpec spec,
                                     MonotonicTimeNs now);
    RetargetResult RetargetDuration(double target, DurationSpec spec);
    RetargetResult RetargetDurationAt(double target, DurationSpec spec, MonotonicTimeNs now);
    RetargetResult RetargetSpring(double target, SpringSpec spec);
    RetargetResult RetargetSpringAt(double target, SpringSpec spec, MonotonicTimeNs now);

    MotionSample Sample();
    MotionSample SampleAt(MonotonicTimeNs now);
    MotionSample PreviewAt(MonotonicTimeNs now) const noexcept;
    std::optional<TerminalEvent> Pause();
    bool Resume();
    std::optional<TerminalEvent> Cancel();

    // A hard completion deadline, not a preferred frame cadence. Paused and
    // terminal trajectories have no active deadline.
    std::optional<MonotonicTimeNs> CompletionDeadlineNs() const noexcept;
    MotionState State() const noexcept;
    std::uint64_t Generation() const noexcept;
    double Target() const noexcept;

private:
    enum class Kind { Duration, Spring };

    MotionSample EvaluateAt(MonotonicTimeNs now) const noexcept;
    bool CanStart() const noexcept;
    void Initialize(double from, double target, double velocity, Kind kind, DurationSpec duration,
                    SpringSpec spring, MonotonicTimeNs now) noexcept;
    RetargetResult Retarget(double target, Kind kind, DurationSpec duration, SpringSpec spring,
                            MonotonicTimeNs now);

    const AnimationClock &clock_;
    Kind kind_{Kind::Duration};
    MotionState state_{MotionState::Idle};
    DurationSpec duration_{};
    SpringSpec spring_{};
    MonotonicTimeNs start_ns_{};
    MonotonicTimeNs pause_ns_{};
    std::uint64_t generation_{};
    double from_{};
    double target_{};
    double initial_velocity_{};
};

} // namespace prism::animation
