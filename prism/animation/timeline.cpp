#include "prism/animation/timeline.hpp"

#include <cmath>
#include <exception>
#include <limits>
#include <time.h>

namespace prism::animation {
namespace {

constexpr double kNanosecondsPerSecond = 1'000'000'000.0;

MonotonicTimeNs SaturatingAdd(MonotonicTimeNs first, MonotonicTimeNs second) noexcept
{
    if (second > std::numeric_limits<MonotonicTimeNs>::max() - first) {
        return std::numeric_limits<MonotonicTimeNs>::max();
    }

    return first + second;
}

bool ValidEasing(Easing easing) noexcept
{
    switch (easing) {
    case Easing::Linear:
    case Easing::EaseInCubic:
    case Easing::EaseOutCubic:
    case Easing::EaseInOutCubic:
        return true;
    }

    return false;
}

bool ValidSpring(const SpringSpec &spec) noexcept
{
    return std::isfinite(spec.angular_frequency_rad_per_sec) &&
           spec.angular_frequency_rad_per_sec > 0.0 &&
           spec.angular_frequency_rad_per_sec <= 10'000.0 && std::isfinite(spec.damping_ratio) &&
           spec.damping_ratio >= 0.0 && spec.damping_ratio <= 10.0 &&
           std::isfinite(spec.position_epsilon) && spec.position_epsilon > 0.0 &&
           std::isfinite(spec.velocity_epsilon) && spec.velocity_epsilon > 0.0 &&
           spec.max_duration_ns > 0;
}

bool SpringSettled(double offset, double velocity, const SpringSpec &spec) noexcept
{
    // This oscillator's energy never increases for nonnegative damping. The
    // radius also bounds future displacement, unlike an instant position test
    // that can finish while an underdamped spring crosses its target.
    const double velocity_distance = velocity / spec.angular_frequency_rad_per_sec;
    return std::abs(velocity) <= spec.velocity_epsilon &&
           std::hypot(offset, velocity_distance) <= spec.position_epsilon;
}

double Ease(Easing easing, double progress) noexcept
{
    switch (easing) {
    case Easing::Linear:
        return progress;
    case Easing::EaseInCubic:
        return progress * progress * progress;
    case Easing::EaseOutCubic: {
        const double remaining = 1.0 - progress;
        return 1.0 - remaining * remaining * remaining;
    }
    case Easing::EaseInOutCubic:
        if (progress < 0.5) {
            return 4.0 * progress * progress * progress;
        }

        return 1.0 - 4.0 * (1.0 - progress) * (1.0 - progress) * (1.0 - progress);
    }

    return progress;
}

double EaseDerivative(Easing easing, double progress) noexcept
{
    switch (easing) {
    case Easing::Linear:
        return 1.0;
    case Easing::EaseInCubic:
        return 3.0 * progress * progress;
    case Easing::EaseOutCubic:
        return 3.0 * (1.0 - progress) * (1.0 - progress);
    case Easing::EaseInOutCubic:
        if (progress < 0.5) {
            return 12.0 * progress * progress;
        }

        return 12.0 * (1.0 - progress) * (1.0 - progress);
    }

    return 1.0;
}

MotionSample DurationValue(double from, double target, const DurationSpec &spec,
                           MonotonicTimeNs elapsed_ns) noexcept
{
    if (from == target) {
        return {target, 0.0, MotionState::Finished, {}};
    }

    if (elapsed_ns < spec.delay_ns) {
        return {from, 0.0, MotionState::Running, {}};
    }

    const MonotonicTimeNs active_ns = elapsed_ns - spec.delay_ns;
    if (spec.duration_ns == 0 || active_ns >= spec.duration_ns) {
        return {target, 0.0, MotionState::Finished, {}};
    }

    const double progress = static_cast<double>(active_ns) / static_cast<double>(spec.duration_ns);
    const double delta = target - from;
    const double value = from + delta * Ease(spec.easing, progress);
    const double velocity = delta * EaseDerivative(spec.easing, progress) * kNanosecondsPerSecond /
                            static_cast<double>(spec.duration_ns);

    if (!std::isfinite(value) || !std::isfinite(velocity)) {
        return {target, 0.0, MotionState::Finished, {}};
    }

    return {value, velocity, MotionState::Running, {}};
}

MotionSample SpringValue(double from, double target, double initial_velocity,
                         const SpringSpec &spec, MonotonicTimeNs elapsed_ns) noexcept
{
    if (elapsed_ns == 0) {
        if (SpringSettled(from - target, initial_velocity, spec)) {
            return {target, 0.0, MotionState::Finished, {}};
        }

        return {from, initial_velocity, MotionState::Running, {}};
    }

    if (elapsed_ns >= spec.max_duration_ns) {
        return {target, 0.0, MotionState::Finished, {}};
    }

    const double displacement = from - target;
    const double seconds = static_cast<double>(elapsed_ns) / kNanosecondsPerSecond;
    const double frequency = spec.angular_frequency_rad_per_sec;
    const double damping = spec.damping_ratio;
    double offset = 0.0;
    double velocity = 0.0;

    if (damping < 1.0) {
        const double decay = damping * frequency;
        const double oscillation = frequency * std::sqrt(1.0 - damping * damping);
        const double sine_factor = (initial_velocity + decay * displacement) / oscillation;
        const double cosine = std::cos(oscillation * seconds);
        const double sine = std::sin(oscillation * seconds);
        const double envelope = std::exp(-decay * seconds);
        const double wave = displacement * cosine + sine_factor * sine;

        offset = envelope * wave;
        velocity = envelope * (-decay * wave - displacement * oscillation * sine +
                               sine_factor * oscillation * cosine);
    } else if (damping == 1.0) {
        const double slope = initial_velocity + frequency * displacement;
        const double envelope = std::exp(-frequency * seconds);
        const double line = displacement + slope * seconds;

        offset = envelope * line;
        velocity = envelope * (slope - frequency * line);
    } else {
        const double root = std::sqrt(damping * damping - 1.0);
        const double slow_rate = -frequency * (damping - root);
        const double fast_rate = -frequency * (damping + root);
        const double slow_weight =
            (initial_velocity - fast_rate * displacement) / (slow_rate - fast_rate);
        const double fast_weight = displacement - slow_weight;
        const double slow_part = slow_weight * std::exp(slow_rate * seconds);
        const double fast_part = fast_weight * std::exp(fast_rate * seconds);

        offset = slow_part + fast_part;
        velocity = slow_rate * slow_part + fast_rate * fast_part;
    }

    const double value = target + offset;
    if (!std::isfinite(value) || !std::isfinite(velocity)) {
        return {target, 0.0, MotionState::Finished, {}};
    }

    if (SpringSettled(offset, velocity, spec)) {
        return {target, 0.0, MotionState::Finished, {}};
    }

    return {value, velocity, MotionState::Running, {}};
}

} // namespace

MonotonicTimeNs SystemAnimationClock::NowNs() const noexcept
{
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        std::terminate();
    }

    return static_cast<MonotonicTimeNs>(now.tv_sec) * 1'000'000'000ULL +
           static_cast<MonotonicTimeNs>(now.tv_nsec);
}

ScalarTimeline::ScalarTimeline(const AnimationClock &clock) noexcept : clock_(clock)
{
}

bool ScalarTimeline::CanStart() const noexcept
{
    return state_ == MotionState::Idle || state_ == MotionState::Finished ||
           state_ == MotionState::Cancelled;
}

void ScalarTimeline::Initialize(double from, double target, double velocity, Kind kind,
                                DurationSpec duration, SpringSpec spring,
                                MonotonicTimeNs now) noexcept
{
    ++generation_;
    kind_ = kind;
    duration_ = duration;
    spring_ = spring;
    start_ns_ = now;
    pause_ns_ = 0;
    from_ = from;
    target_ = target;
    initial_velocity_ = velocity;
    state_ = MotionState::Running;
}

bool ScalarTimeline::StartDuration(double from, double target, DurationSpec spec)
{
    return StartDurationAt(from, target, spec, clock_.NowNs());
}

bool ScalarTimeline::StartDurationAt(double from, double target, DurationSpec spec,
                                     MonotonicTimeNs now)
{
    if (!CanStart() || generation_ == std::numeric_limits<std::uint64_t>::max() ||
        !std::isfinite(from) || !std::isfinite(target) || !std::isfinite(target - from) ||
        !ValidEasing(spec.easing)) {
        return false;
    }

    Initialize(from, target, 0.0, Kind::Duration, spec, {}, now);

    return true;
}

bool ScalarTimeline::StartSpring(double from, double target, double initial_velocity,
                                 SpringSpec spec)
{
    return StartSpringAt(from, target, initial_velocity, spec, clock_.NowNs());
}

bool ScalarTimeline::StartSpringAt(double from, double target, double initial_velocity,
                                   SpringSpec spec, MonotonicTimeNs now)
{
    if (!CanStart() || generation_ == std::numeric_limits<std::uint64_t>::max() ||
        !std::isfinite(from) || !std::isfinite(target) || !std::isfinite(target - from) ||
        !std::isfinite(initial_velocity) || !ValidSpring(spec)) {
        return false;
    }

    Initialize(from, target, initial_velocity, Kind::Spring, {}, spec, now);

    return true;
}

MotionSample ScalarTimeline::EvaluateAt(MonotonicTimeNs now) const noexcept
{
    if (state_ == MotionState::Idle) {
        return {};
    }
    if (state_ == MotionState::Finished || state_ == MotionState::Cancelled) {
        return {target_, 0.0, state_, {}};
    }

    const MonotonicTimeNs sampled_now = state_ == MotionState::Paused ? pause_ns_ : now;
    const MonotonicTimeNs elapsed = sampled_now >= start_ns_ ? sampled_now - start_ns_ : 0;
    if (kind_ == Kind::Duration) {
        return DurationValue(from_, target_, duration_, elapsed);
    }

    return SpringValue(from_, target_, initial_velocity_, spring_, elapsed);
}

MotionSample ScalarTimeline::Sample()
{
    return SampleAt(clock_.NowNs());
}

MotionSample ScalarTimeline::PreviewAt(MonotonicTimeNs now) const noexcept
{
    MotionSample sample = EvaluateAt(now);
    if (state_ == MotionState::Paused && sample.state == MotionState::Running) {
        sample.state = MotionState::Paused;
    }

    return sample;
}

MotionSample ScalarTimeline::SampleAt(MonotonicTimeNs now)
{
    MotionSample sample = EvaluateAt(now);
    if (sample.state == MotionState::Finished && state_ != MotionState::Finished) {
        state_ = MotionState::Finished;
        sample.terminal = TerminalEvent{generation_, TerminalReason::Finished};
    } else if (state_ == MotionState::Paused && sample.state == MotionState::Running) {
        sample.state = MotionState::Paused;
    }

    return sample;
}

RetargetResult ScalarTimeline::RestartDuration(double from, double target, DurationSpec spec)
{
    return RestartDurationAt(from, target, spec, clock_.NowNs());
}

RetargetResult ScalarTimeline::RestartDurationAt(double from, double target, DurationSpec spec,
                                                 MonotonicTimeNs now)
{
    if (state_ == MotionState::Idle || generation_ == std::numeric_limits<std::uint64_t>::max() ||
        !std::isfinite(from) || !std::isfinite(target) || !std::isfinite(target - from) ||
        !ValidEasing(spec.easing)) {
        return {};
    }

    const MotionSample previous = SampleAt(now);
    std::optional<TerminalEvent> terminal = previous.terminal;
    if (!terminal && (state_ == MotionState::Running || state_ == MotionState::Paused)) {
        terminal = TerminalEvent{generation_, TerminalReason::Superseded};
    }

    const bool was_paused = state_ == MotionState::Paused;
    Initialize(from, target, 0.0, Kind::Duration, spec, {}, now);
    if (was_paused) {
        state_ = MotionState::Paused;
        pause_ns_ = now;
    }

    return {true, terminal};
}

RetargetResult ScalarTimeline::Retarget(double target, Kind kind, DurationSpec duration,
                                        SpringSpec spring, MonotonicTimeNs now)
{
    const MotionSample previous = SampleAt(now);
    if (target == target_) {
        return {true, previous.terminal};
    }

    std::optional<TerminalEvent> terminal = previous.terminal;
    if (!terminal && (state_ == MotionState::Running || state_ == MotionState::Paused)) {
        terminal = TerminalEvent{generation_, TerminalReason::Superseded};
    }

    if (target == previous.value) {
        state_ = MotionState::Finished;
        target_ = target;

        return {true, terminal};
    }

    const bool was_paused = state_ == MotionState::Paused;
    Initialize(previous.value, target, previous.velocity, kind, duration, spring, now);
    if (was_paused) {
        state_ = MotionState::Paused;
        pause_ns_ = now;
    }

    return {true, terminal};
}

RetargetResult ScalarTimeline::RetargetDuration(double target, DurationSpec spec)
{
    return RetargetDurationAt(target, spec, clock_.NowNs());
}

RetargetResult ScalarTimeline::RetargetDurationAt(double target, DurationSpec spec,
                                                  MonotonicTimeNs now)
{
    if (state_ == MotionState::Idle || generation_ == std::numeric_limits<std::uint64_t>::max() ||
        !std::isfinite(target) || !std::isfinite(target - EvaluateAt(now).value) ||
        !ValidEasing(spec.easing)) {
        return {};
    }

    return Retarget(target, Kind::Duration, spec, {}, now);
}

RetargetResult ScalarTimeline::RetargetSpring(double target, SpringSpec spec)
{
    return RetargetSpringAt(target, spec, clock_.NowNs());
}

RetargetResult ScalarTimeline::RetargetSpringAt(double target, SpringSpec spec, MonotonicTimeNs now)
{
    if (state_ == MotionState::Idle || generation_ == std::numeric_limits<std::uint64_t>::max() ||
        !std::isfinite(target) || !std::isfinite(target - EvaluateAt(now).value) ||
        !ValidSpring(spec)) {
        return {};
    }

    return Retarget(target, Kind::Spring, {}, spec, now);
}

std::optional<TerminalEvent> ScalarTimeline::Pause()
{
    if (state_ != MotionState::Running) {
        return {};
    }

    const MonotonicTimeNs now = clock_.NowNs();
    const MotionSample sample = SampleAt(now);
    if (sample.state == MotionState::Finished) {
        return sample.terminal;
    }

    state_ = MotionState::Paused;
    pause_ns_ = now;

    return {};
}

bool ScalarTimeline::Resume()
{
    if (state_ != MotionState::Paused) {
        return false;
    }

    const MonotonicTimeNs now = clock_.NowNs();
    if (now > pause_ns_) {
        start_ns_ = SaturatingAdd(start_ns_, now - pause_ns_);
    }
    pause_ns_ = 0;
    state_ = MotionState::Running;

    return true;
}

std::optional<TerminalEvent> ScalarTimeline::Cancel()
{
    if (state_ != MotionState::Running && state_ != MotionState::Paused) {
        return {};
    }

    const MotionSample sample = SampleAt(clock_.NowNs());
    if (sample.terminal) {
        return sample.terminal;
    }

    state_ = MotionState::Cancelled;

    return TerminalEvent{generation_, TerminalReason::Cancelled};
}

std::optional<MonotonicTimeNs> ScalarTimeline::CompletionDeadlineNs() const noexcept
{
    if (state_ != MotionState::Running) {
        return {};
    }

    if (kind_ == Kind::Duration) {
        return SaturatingAdd(SaturatingAdd(start_ns_, duration_.delay_ns), duration_.duration_ns);
    }

    return SaturatingAdd(start_ns_, spring_.max_duration_ns);
}

MotionState ScalarTimeline::State() const noexcept
{
    return state_;
}

std::uint64_t ScalarTimeline::Generation() const noexcept
{
    return generation_;
}

double ScalarTimeline::Target() const noexcept
{
    return target_;
}

} // namespace prism::animation
