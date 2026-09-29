#include "prism/animation/timeline.hpp"

#include <cassert>
#include <cmath>

namespace {

using namespace prism::animation;

constexpr MonotonicTimeNs kMillisecond = 1'000'000;

class ManualClock final : public AnimationClock {
public:
    MonotonicTimeNs NowNs() const noexcept override
    {
        return now_;
    }

    void SetMs(MonotonicTimeNs milliseconds)
    {
        now_ = milliseconds * kMillisecond;
    }

private:
    MonotonicTimeNs now_{};
};

bool Near(double actual, double expected, double tolerance = 0.000001)
{
    return std::abs(actual - expected) <= tolerance;
}

void TestDurationDependsOnTime()
{
    ManualClock clock;
    ScalarTimeline dense(clock);
    ScalarTimeline sparse(clock);
    const DurationSpec spec{180 * kMillisecond, 0, Easing::EaseOutCubic};
    assert(dense.StartDuration(0.2, 0.8, spec));
    assert(sparse.StartDuration(0.2, 0.8, spec));

    clock.SetMs(16);
    assert(dense.Sample().state == MotionState::Running);
    clock.SetMs(33);
    assert(dense.Sample().state == MotionState::Running);
    clock.SetMs(90);
    const MotionSample dense_half = dense.Sample();
    const MotionSample sparse_half = sparse.Sample();
    assert(Near(dense_half.value, 0.725));
    assert(Near(dense_half.value, sparse_half.value));
    assert(Near(dense_half.velocity, sparse_half.velocity));
    assert(dense.CompletionDeadlineNs() == 180 * kMillisecond);

    clock.SetMs(200);
    const MotionSample final = dense.Sample();
    assert(final.value == 0.8);
    assert(final.velocity == 0.0);
    assert(final.terminal && final.terminal->reason == TerminalReason::Finished);
    assert(!dense.CompletionDeadlineNs());
    assert(!dense.Sample().terminal);
    assert(sparse.Sample().value == 0.8);
}

void TestDurationCurvesAndDelay()
{
    ManualClock clock;
    ScalarTimeline timeline(clock);
    assert(timeline.StartDuration(0.0, 1.0,
                                  {100 * kMillisecond, 20 * kMillisecond, Easing::EaseInOutCubic}));
    assert(timeline.CompletionDeadlineNs() == 120 * kMillisecond);

    clock.SetMs(10);
    assert(timeline.Sample().value == 0.0);
    clock.SetMs(45);
    assert(Near(timeline.Sample().value, 0.0625));
    clock.SetMs(70);
    assert(Near(timeline.Sample().value, 0.5));
    clock.SetMs(95);
    assert(Near(timeline.Sample().value, 0.9375));
    clock.SetMs(120);
    assert(timeline.Sample().value == 1.0);

    ScalarTimeline instant(clock);
    assert(instant.StartDuration(4.0, 9.0, {0, 0, Easing::Linear}));
    const MotionSample snapped = instant.Sample();
    assert(snapped.value == 9.0 && snapped.terminal);
    assert(!instant.Sample().terminal);
}

void TestRetargetAndRestart()
{
    ManualClock clock;
    ScalarTimeline timeline(clock);
    assert(timeline.StartDuration(0.0, 10.0, {100 * kMillisecond, 0, Easing::Linear}));

    clock.SetMs(40);
    const MotionSample before = timeline.Sample();
    assert(Near(before.value, 4.0));
    assert(Near(before.velocity, 100.0));
    const RetargetResult next =
        timeline.RetargetDuration(20.0, {100 * kMillisecond, 0, Easing::Linear});
    assert(next.accepted && next.previous_terminal);
    assert(next.previous_terminal->generation == 1);
    assert(next.previous_terminal->reason == TerminalReason::Superseded);
    assert(timeline.Generation() == 2);
    assert(Near(timeline.Sample().value, 4.0));

    clock.SetMs(90);
    assert(Near(timeline.Sample().value, 12.0));
    const RetargetResult same =
        timeline.RetargetDuration(20.0, {100 * kMillisecond, 0, Easing::Linear});
    assert(same.accepted && !same.previous_terminal && timeline.Generation() == 2);

    const RetargetResult restarted =
        timeline.RestartDuration(0.0, 1.0, {200 * kMillisecond, 0, Easing::Linear});
    assert(restarted.accepted && restarted.previous_terminal);
    assert(restarted.previous_terminal->reason == TerminalReason::Superseded);
    assert(timeline.Generation() == 3);
    assert(timeline.Sample().value == 0.0);
    clock.SetMs(190);
    assert(Near(timeline.Sample().value, 0.5));
}

void TestBatchTimestampAndPreview()
{
    ManualClock clock;
    ScalarTimeline first(clock);
    ScalarTimeline second(clock);
    clock.SetMs(50);
    assert(first.StartDurationAt(0.0, 1.0, {100 * kMillisecond, 0, Easing::Linear},
                                 10 * kMillisecond));
    assert(second.StartDurationAt(0.0, 1.0, {100 * kMillisecond, 0, Easing::Linear},
                                  10 * kMillisecond));
    assert(first.CompletionDeadlineNs() == 110 * kMillisecond);
    assert(Near(first.SampleAt(50 * kMillisecond).value, 0.4));
    assert(Near(second.SampleAt(50 * kMillisecond).value, 0.4));

    const MotionSample preview = first.PreviewAt(120 * kMillisecond);
    assert(preview.value == 1.0 && preview.state == MotionState::Finished);
    assert(!preview.terminal && first.State() == MotionState::Running);
    const MotionSample final = first.SampleAt(120 * kMillisecond);
    assert(final.terminal && final.terminal->reason == TerminalReason::Finished);
    assert(!first.SampleAt(120 * kMillisecond).terminal);
}

void TestPauseCancelAndValidation()
{
    ManualClock clock;
    ScalarTimeline timeline(clock);
    assert(timeline.StartDuration(0.0, 10.0, {100 * kMillisecond, 0, Easing::Linear}));

    clock.SetMs(30);
    assert(!timeline.Pause());
    assert(timeline.State() == MotionState::Paused);
    assert(!timeline.CompletionDeadlineNs());
    clock.SetMs(150);
    assert(Near(timeline.Sample().value, 3.0));
    assert(timeline.Resume());
    assert(timeline.CompletionDeadlineNs() == 220 * kMillisecond);
    clock.SetMs(170);
    assert(Near(timeline.Sample().value, 5.0));

    const auto bad_easing = static_cast<Easing>(99);
    assert(!timeline.RetargetDuration(12.0, {100 * kMillisecond, 0, bad_easing}).accepted);
    assert(timeline.Generation() == 1);
    assert(timeline.Target() == 10.0);

    const auto cancelled = timeline.Cancel();
    assert(cancelled && cancelled->generation == 1);
    assert(cancelled->reason == TerminalReason::Cancelled);
    assert(timeline.State() == MotionState::Cancelled);
    assert(timeline.Sample().value == 10.0);
    assert(!timeline.Sample().terminal && !timeline.Cancel());
    assert(!timeline.CompletionDeadlineNs());
}

void TestAnalyticSpringAndVelocityRetarget()
{
    ManualClock clock;
    ScalarTimeline timeline(clock);
    ScalarTimeline sparse(clock);
    SpringSpec spring{};
    spring.angular_frequency_rad_per_sec = 10.0;
    spring.damping_ratio = 1.0;
    spring.position_epsilon = 0.0000001;
    spring.velocity_epsilon = 0.0000001;
    spring.max_duration_ns = 1'000 * kMillisecond;
    assert(timeline.StartSpring(0.0, 1.0, 0.0, spring));
    assert(sparse.StartSpring(0.0, 1.0, 0.0, spring));

    clock.SetMs(16);
    assert(timeline.Sample().state == MotionState::Running);
    clock.SetMs(33);
    assert(timeline.Sample().state == MotionState::Running);
    clock.SetMs(100);
    const MotionSample before = timeline.Sample();
    const MotionSample sparse_before = sparse.Sample();
    assert(Near(before.value, 1.0 - 2.0 / std::exp(1.0)));
    assert(Near(before.velocity, 10.0 / std::exp(1.0)));
    assert(Near(before.value, sparse_before.value));
    assert(Near(before.velocity, sparse_before.velocity));
    const RetargetResult next = timeline.RetargetSpring(0.0, spring);
    assert(next.accepted && next.previous_terminal);
    assert(next.previous_terminal->reason == TerminalReason::Superseded);
    const MotionSample after = timeline.Sample();
    assert(Near(after.value, before.value));
    assert(Near(after.velocity, before.velocity));

    clock.SetMs(1'100);
    const MotionSample final = timeline.Sample();
    assert(final.value == 0.0);
    assert(final.terminal && final.terminal->generation == 2);
    assert(!timeline.Sample().terminal);

    ScalarTimeline underdamped(clock);
    spring.damping_ratio = 0.4;
    assert(underdamped.StartSpring(0.0, 1.0, 0.0, spring));
    clock.SetMs(1'150);
    assert(std::isfinite(underdamped.Sample().value));

    ScalarTimeline overdamped(clock);
    spring.damping_ratio = 2.0;
    assert(overdamped.StartSpring(0.0, 1.0, 0.0, spring));
    clock.SetMs(1'200);
    assert(std::isfinite(overdamped.Sample().value));

    ScalarTimeline invalid(clock);
    spring.angular_frequency_rad_per_sec = 0.0;
    assert(!invalid.StartSpring(0.0, 1.0, 0.0, spring));
    assert(invalid.State() == MotionState::Idle);
}

void TestSpringCannotSettleWhileCrossingTarget()
{
    ManualClock clock;
    ScalarTimeline dense(clock);
    ScalarTimeline sparse(clock);
    SpringSpec spring{};
    spring.angular_frequency_rad_per_sec = 10.0;
    spring.damping_ratio = 0.1;
    spring.position_epsilon = 0.02;
    spring.velocity_epsilon = 100.0;
    spring.max_duration_ns = 1'000 * kMillisecond;
    assert(dense.StartSpring(0.0, 1.0, 0.0, spring));
    assert(sparse.StartSpring(0.0, 1.0, 0.0, spring));

    for (MonotonicTimeNs millisecond = 1; millisecond <= 300; ++millisecond) {
        clock.SetMs(millisecond);
        dense.Sample();
    }

    const MotionSample at_300_dense = dense.Sample();
    const MotionSample at_300_sparse = sparse.Sample();
    assert(at_300_dense.state == MotionState::Running);
    assert(Near(at_300_dense.value, at_300_sparse.value));
    assert(Near(at_300_dense.velocity, at_300_sparse.velocity));
}

} // namespace

int main()
{
    TestDurationDependsOnTime();
    TestDurationCurvesAndDelay();
    TestRetargetAndRestart();
    TestBatchTimestampAndPreview();
    TestPauseCancelAndValidation();
    TestAnalyticSpringAndVelocityRetarget();
    TestSpringCannotSettleWhileCrossingTarget();
}
