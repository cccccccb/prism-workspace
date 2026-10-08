#include "prism/runtime/task_motion.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

using namespace prism;

namespace {
using Endpoint = runtime::TaskPresentationEndpoint;
using Kind = runtime::TaskPresentationSampleKind;
using animation::Easing;
using animation::MonotonicTimeNs;
constexpr MonotonicTimeNs millisecond = 1'000'000;

class ManualClock final : public animation::AnimationClock {
public:
    MonotonicTimeNs NowNs() const noexcept override
    {
        ++reads;
        return now;
    }

    MonotonicTimeNs now{};
    mutable std::uint64_t reads{};
};

runtime::TaskMotionSpec Linear(MonotonicTimeNs opening_ms = 100, MonotonicTimeNs closing_ms = 100)
{
    return {{opening_ms * millisecond, 0, Easing::Linear},
            {closing_ms * millisecond, 0, Easing::Linear}};
}

void Near(double actual, double expected)
{
    assert(std::abs(actual - expected) <= 0.0000001);
}

void Check(const runtime::TaskMotionSample &sample, double reveal, Endpoint endpoint, Kind kind,
           MonotonicTimeNs time_ns)
{
    Near(sample.reveal, reveal);
    assert(sample.endpoint == endpoint && sample.kind == kind && sample.time_ns == time_ns);
}

void Instant(const animation::DurationSpec &spec)
{
    assert(!spec.duration_ns && !spec.delay_ns && spec.easing == Easing::Linear);
}

void CheckNamedRecipeResolution()
{
    const contracts::MotionSet set{"custom",
                                   {{"control.feedback", 55, contracts::MotionEasing::Linear},
                                    {"task.close", 120, contracts::MotionEasing::EaseInOutCubic},
                                    {"task.open", 180, contracts::MotionEasing::EaseOutCubic}}};
    const auto spec = runtime::ResolveTaskMotionSpec(set);
    assert(spec.opening.duration_ns == 180 * millisecond && !spec.opening.delay_ns);
    assert(spec.opening.easing == Easing::EaseOutCubic);
    assert(spec.closing.duration_ns == 120 * millisecond && !spec.closing.delay_ns);
    assert(spec.closing.easing == Easing::EaseInOutCubic);
}

void CheckMissingRecipesAndLegacyAreInstant()
{
    const auto empty = runtime::ResolveTaskMotionSpec({});
    Instant(empty.opening);
    Instant(empty.closing);
    const auto legacy = runtime::ResolveTaskMotionSpec(
        {"legacy", {{"popup.open", 120, contracts::MotionEasing::EaseOutCubic}}});
    Instant(legacy.opening);
    Instant(legacy.closing);

    const auto opening = runtime::ResolveTaskMotionSpec(
        {"custom", {{"task.open", 180, contracts::MotionEasing::EaseInCubic}}});
    assert(opening.opening.duration_ns == 180 * millisecond);
    assert(opening.opening.easing == Easing::EaseInCubic);
    Instant(opening.closing);
    const auto closing = runtime::ResolveTaskMotionSpec(
        {"custom", {{"task.close", 120, contracts::MotionEasing::EaseOutCubic}}});
    Instant(closing.opening);
    assert(closing.closing.duration_ns == 120 * millisecond);
    assert(closing.closing.easing == Easing::EaseOutCubic);
}

void CheckProfileIdentityDoesNotSelectMotion()
{
    const auto misleading =
        runtime::ResolveTaskMotionSpec({"instant",
                                        {{"task.open", 210, contracts::MotionEasing::Linear},
                                         {"task.close", 140, contracts::MotionEasing::Linear}}});
    assert(misleading.opening.duration_ns == 210 * millisecond);
    assert(misleading.closing.duration_ns == 140 * millisecond);

    const auto zero =
        runtime::ResolveTaskMotionSpec({"custom",
                                        {{"task.open", 0, contracts::MotionEasing::Linear},
                                         {"task.close", 0, contracts::MotionEasing::Linear}}});
    Instant(zero.opening);
    Instant(zero.closing);
}

void Reject(const contracts::MotionSet &set)
{
    bool rejected = false;
    try {
        runtime::ResolveTaskMotionSpec(set);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
}

void CheckMalformedRecipeContractsAreRejected()
{
    Reject({"custom", {}});
    Reject({"", {{"task.open", 100, contracts::MotionEasing::Linear}}});
    Reject({"bad.id", {{"task.open", 100, contracts::MotionEasing::Linear}}});
    Reject({"custom",
            {{"task.open", 100, contracts::MotionEasing::Linear},
             {"task.open", 100, contracts::MotionEasing::Linear}}});
    Reject({"custom", {{"task.close", 10001, contracts::MotionEasing::Linear}}});
    Reject({"custom", {{"task.open", 100, static_cast<contracts::MotionEasing>(99)}}});
}

void CheckIdleAndZeroDurationEndpoints()
{
    ManualClock clock;
    runtime::TaskMotionTimeline motion(clock);
    Check(motion.SampleAt(7), 0, Endpoint::Closed, Kind::Terminal, 7);
    assert(!motion.IsActive() && !motion.CompletionDeadlineNs());
    assert(motion.BeginOpen({}, 10));
    assert(!motion.IsActive() && !motion.CompletionDeadlineNs());
    Check(motion.SampleAt(10), 1, Endpoint::Open, Kind::Terminal, 10);
    Check(motion.SampleAt(12), 1, Endpoint::Open, Kind::Terminal, 12);
    assert(motion.BeginClose({}, 0.35, 20));
    assert(!motion.IsActive() && !motion.CompletionDeadlineNs());
    Check(motion.SampleAt(20), 0, Endpoint::Closed, Kind::Terminal, 20);
    Check(motion.SampleAt(25), 0, Endpoint::Closed, Kind::Terminal, 25);

    const runtime::TaskMotionSpec delayed{{100 * millisecond, 20 * millisecond, Easing::Linear},
                                          {100 * millisecond, 20 * millisecond, Easing::Linear}};
    assert(motion.BeginClose(delayed, 0, 30));
    assert(!motion.IsActive() && !motion.CompletionDeadlineNs());
    Check(motion.SampleAt(30), 0, Endpoint::Closed, Kind::Terminal, 30);
}

void CheckDenseAndSparseSamplingAgree()
{
    ManualClock clock;
    clock.now = 999 * millisecond;
    runtime::TaskMotionTimeline dense(clock);
    runtime::TaskMotionTimeline sparse(clock);
    assert(dense.BeginOpen(Linear(), 10 * millisecond));
    assert(sparse.BeginOpen(Linear(), 10 * millisecond));
    for (const auto ms : {20, 30, 45}) {
        assert(dense.SampleAt(ms * millisecond).kind == Kind::Intermediate);
    }
    const auto frequent = dense.SampleAt(60 * millisecond);
    const auto infrequent = sparse.SampleAt(60 * millisecond);
    assert(frequent == infrequent);
    Check(frequent, 0.5, Endpoint::Open, Kind::Intermediate, 60 * millisecond);
    assert(dense.CompletionDeadlineNs() == 110 * millisecond);
    assert(sparse.CompletionDeadlineNs() == dense.CompletionDeadlineNs());
    for (const auto ms : {70, 80, 95}) {
        assert(dense.SampleAt(ms * millisecond).kind == Kind::Intermediate);
    }
    const auto frequent_end = dense.SampleAt(150 * millisecond);
    const auto sparse_end = sparse.SampleAt(150 * millisecond);
    assert(frequent_end == sparse_end);
    Check(sparse_end, 1, Endpoint::Open, Kind::Terminal, 150 * millisecond);
    assert(!dense.IsActive() && !sparse.IsActive());
    assert(!dense.CompletionDeadlineNs() && !sparse.CompletionDeadlineNs());
    assert(!clock.reads);
}

void CheckAbsoluteTimeAndTerminalStickiness()
{
    ManualClock clock;
    runtime::TaskMotionTimeline motion(clock);
    assert(motion.BeginOpen(Linear(), 100 * millisecond));
    Check(motion.SampleAt(150 * millisecond), 0.5, Endpoint::Open, Kind::Intermediate,
          150 * millisecond);
    Check(motion.SampleAt(125 * millisecond), 0.25, Endpoint::Open, Kind::Intermediate,
          125 * millisecond);
    Check(motion.SampleAt(75 * millisecond), 0, Endpoint::Open, Kind::Intermediate,
          75 * millisecond);
    Check(motion.SampleAt(150 * millisecond), 0.5, Endpoint::Open, Kind::Intermediate,
          150 * millisecond);
    Check(motion.SampleAt(200 * millisecond), 1, Endpoint::Open, Kind::Terminal, 200 * millisecond);
    // Scalar's finished state is sticky. A stale timestamp cannot revive an
    // already finished track or manufacture another intermediate endpoint.
    Check(motion.SampleAt(175 * millisecond), 1, Endpoint::Open, Kind::Terminal, 175 * millisecond);
    Check(motion.SampleAt(200 * millisecond), 1, Endpoint::Open, Kind::Terminal, 200 * millisecond);
    assert(!motion.IsActive() && !motion.CompletionDeadlineNs());
}

void CheckClosingStartsFromAdoptedReveal()
{
    ManualClock clock;
    runtime::TaskMotionTimeline motion(clock);
    const auto spec = Linear(200, 100);
    assert(motion.BeginOpen(spec, 10 * millisecond));
    Check(motion.SampleAt(160 * millisecond), 0.75, Endpoint::Open, Kind::Intermediate,
          160 * millisecond);

    // The most recent computed opening value was not adopted. Close begins
    // from the last seen 0.25 supplied by its caller, rather than that 0.75.
    assert(motion.BeginClose(spec, 0.25, 160 * millisecond));
    Check(motion.SampleAt(160 * millisecond), 0.25, Endpoint::Closed, Kind::Intermediate,
          160 * millisecond);
    assert(motion.CompletionDeadlineNs() == 260 * millisecond);
    Check(motion.SampleAt(210 * millisecond), 0.125, Endpoint::Closed, Kind::Intermediate,
          210 * millisecond);
    Check(motion.SampleAt(300 * millisecond), 0, Endpoint::Closed, Kind::Terminal,
          300 * millisecond);
    assert(!motion.IsActive() && !motion.CompletionDeadlineNs());
}

void CheckNewOpeningSupersedesClosing()
{
    ManualClock clock;
    runtime::TaskMotionTimeline motion(clock);
    const auto spec = Linear();
    assert(motion.BeginClose(spec, 1, 100 * millisecond));
    Check(motion.SampleAt(125 * millisecond), 0.75, Endpoint::Closed, Kind::Intermediate,
          125 * millisecond);
    assert(motion.BeginOpen(spec, 125 * millisecond));
    Check(motion.SampleAt(125 * millisecond), 0, Endpoint::Open, Kind::Intermediate,
          125 * millisecond);
    assert(motion.CompletionDeadlineNs() == 225 * millisecond);
    Check(motion.SampleAt(175 * millisecond), 0.5, Endpoint::Open, Kind::Intermediate,
          175 * millisecond);
    Check(motion.SampleAt(225 * millisecond), 1, Endpoint::Open, Kind::Terminal, 225 * millisecond);
}

void CheckEasingAndDelay()
{
    constexpr std::array easings{Easing::Linear, Easing::EaseInCubic, Easing::EaseOutCubic,
                                 Easing::EaseInOutCubic};
    constexpr std::array halfway{0.5, 0.125, 0.875, 0.5};
    for (std::size_t i = 0; i < easings.size(); ++i) {
        ManualClock clock;
        runtime::TaskMotionTimeline motion(clock);
        const runtime::TaskMotionSpec spec{{100 * millisecond, 20 * millisecond, easings[i]},
                                           {100 * millisecond, 20 * millisecond, easings[i]}};
        assert(motion.BeginOpen(spec, 100 * millisecond));
        assert(motion.CompletionDeadlineNs() == 220 * millisecond);
        Check(motion.SampleAt(110 * millisecond), 0, Endpoint::Open, Kind::Intermediate,
              110 * millisecond);
        Check(motion.SampleAt(120 * millisecond), 0, Endpoint::Open, Kind::Intermediate,
              120 * millisecond);
        Check(motion.SampleAt(170 * millisecond), halfway[i], Endpoint::Open, Kind::Intermediate,
              170 * millisecond);
        Check(motion.SampleAt(220 * millisecond), 1, Endpoint::Open, Kind::Terminal,
              220 * millisecond);
        assert(motion.BeginClose(spec, 0.6, 250 * millisecond));
        Check(motion.SampleAt(260 * millisecond), 0.6, Endpoint::Closed, Kind::Intermediate,
              260 * millisecond);
        Check(motion.SampleAt(320 * millisecond), 0.6 * (1 - halfway[i]), Endpoint::Closed,
              Kind::Intermediate, 320 * millisecond);
        Check(motion.SampleAt(370 * millisecond), 0, Endpoint::Closed, Kind::Terminal,
              370 * millisecond);
    }
}

void CheckInvalidStartsPreserveRunningTrack()
{
    ManualClock clock;
    runtime::TaskMotionTimeline motion(clock);
    const auto spec = Linear();
    assert(motion.BeginOpen(spec, 0));
    const auto unchanged = motion.SampleAt(40 * millisecond);
    const auto deadline = motion.CompletionDeadlineNs();
    const auto bad_easing = static_cast<Easing>(99);
    auto bad = spec;
    bad.opening.easing = bad_easing;
    bad.closing.easing = bad_easing;

    // The rejected timestamp is past the old deadline. Validation must happen
    // before sampling/restarting that track, preserving its running state.
    assert(!motion.BeginOpen(bad, 200 * millisecond));
    assert(!motion.BeginClose(bad, 0.5, 200 * millisecond));
    for (const double from :
         {-0.1, 1.1, std::numeric_limits<double>::quiet_NaN(),
          std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
        assert(!motion.BeginClose(spec, from, 200 * millisecond));
    }
    assert(motion.IsActive() && motion.CompletionDeadlineNs() == deadline);
    assert(motion.SampleAt(40 * millisecond) == unchanged);
    Check(motion.SampleAt(80 * millisecond), 0.8, Endpoint::Open, Kind::Intermediate,
          80 * millisecond);
}

void CheckOnlyTheSelectedDirectionIsValidated()
{
    ManualClock clock;
    runtime::TaskMotionTimeline motion(clock);
    auto spec = Linear();
    spec.closing.easing = static_cast<Easing>(99);
    assert(motion.BeginOpen(spec, 0));
    Check(motion.SampleAt(50 * millisecond), 0.5, Endpoint::Open, Kind::Intermediate,
          50 * millisecond);
    spec = Linear();
    spec.opening.easing = static_cast<Easing>(99);
    assert(motion.BeginClose(spec, 0.5, 50 * millisecond));
    Check(motion.SampleAt(100 * millisecond), 0.25, Endpoint::Closed, Kind::Intermediate,
          100 * millisecond);

    auto bad_instant = runtime::TaskMotionSpec{};
    bad_instant.closing.easing = static_cast<Easing>(99);
    assert(!motion.BeginClose(bad_instant, 0, 300 * millisecond));
    assert(motion.IsActive());
    Check(motion.SampleAt(100 * millisecond), 0.25, Endpoint::Closed, Kind::Intermediate,
          100 * millisecond);
}

void CheckDeadlineSaturationMatchesScalarContract()
{
    ManualClock clock;
    runtime::TaskMotionTimeline motion(clock);
    constexpr auto maximum = std::numeric_limits<MonotonicTimeNs>::max();
    const runtime::TaskMotionSpec spec{{100, 40, Easing::Linear}, {100, 40, Easing::Linear}};
    assert(motion.BeginOpen(spec, maximum - 50));
    assert(motion.CompletionDeadlineNs() == maximum);
    Check(motion.SampleAt(maximum), 0.1, Endpoint::Open, Kind::Intermediate, maximum);
    // A saturated deadline does not imply an invented endpoint. Scalar's
    // remaining interval is beyond representable time and is preserved here.
    assert(motion.IsActive() && motion.CompletionDeadlineNs() == maximum);
    assert(motion.BeginClose(spec, 0.5, maximum - 50));
    assert(motion.CompletionDeadlineNs() == maximum);
    Check(motion.SampleAt(maximum), 0.45, Endpoint::Closed, Kind::Intermediate, maximum);
}

void CheckCounterExhaustionPreservesTrack()
{
    ManualClock clock;
    runtime::TaskMotionTimeline motion(clock);
    const auto spec = Linear();
    assert(motion.BeginOpen(spec, 0));
    const auto unchanged = motion.SampleAt(40 * millisecond);
    motion.timeline_.generation_ = std::numeric_limits<std::uint64_t>::max();
    assert(!motion.BeginClose(spec, 0.4, 200 * millisecond));
    assert(!motion.BeginOpen(spec, 200 * millisecond));
    assert(motion.timeline_.Generation() == std::numeric_limits<std::uint64_t>::max());
    assert(motion.SampleAt(40 * millisecond) == unchanged);
    assert(motion.CompletionDeadlineNs() == 100 * millisecond);
    assert(motion.IsActive());

    runtime::TaskMotionTimeline idle(clock);
    idle.timeline_.generation_ = std::numeric_limits<std::uint64_t>::max();
    assert(!idle.BeginOpen(spec, 0) && !idle.BeginClose(spec, 0.5, 0));
    Check(idle.SampleAt(1), 0, Endpoint::Closed, Kind::Terminal, 1);
    assert(!idle.IsActive() && !idle.CompletionDeadlineNs());
}

void CheckBorrowedClockAndExplicitSamples()
{
    ManualClock clock;
    runtime::TaskMotionTimeline motion(clock);
    const auto spec = Linear();
    clock.now = 90 * millisecond;
    assert(motion.BeginOpen(spec, 10 * millisecond));
    Check(motion.SampleAt(30 * millisecond), 0.2, Endpoint::Open, Kind::Intermediate,
          30 * millisecond);
    assert(!clock.reads);
    Check(motion.Sample(), 0.8, Endpoint::Open, Kind::Intermediate, 90 * millisecond);
    assert(clock.reads == 1);
    assert(motion.BeginClose(spec, 0.2, 100 * millisecond));
    assert(clock.reads == 1);
    clock.now = 150 * millisecond;
    Check(motion.Sample(), 0.1, Endpoint::Closed, Kind::Intermediate, 150 * millisecond);
    assert(clock.reads == 2);
    clock.now = 250 * millisecond;
    Check(motion.Sample(), 0, Endpoint::Closed, Kind::Terminal, 250 * millisecond);
    assert(clock.reads == 3 && !motion.IsActive());
}

void Run(std::string_view name, void (*check)())
{
    check();
    std::cout << "PASS " << name << '\n';
}
} // namespace

int main()
{
    Run("named task recipes resolve independently", CheckNamedRecipeResolution);
    Run("missing task entries and legacy are instant", CheckMissingRecipesAndLegacyAreInstant);
    Run("profile identity never selects duration", CheckProfileIdentityDoesNotSelectMotion);
    Run("malformed nonempty motion contracts reject", CheckMalformedRecipeContractsAreRejected);
    Run("idle and zero-duration samples are terminal", CheckIdleAndZeroDurationEndpoints);
    Run("dense and sparse samples agree", CheckDenseAndSparseSamplingAgree);
    Run("absolute time and terminal stickiness", CheckAbsoluteTimeAndTerminalStickiness);
    Run("close starts from actually adopted reveal", CheckClosingStartsFromAdoptedReveal);
    Run("new opening supersedes closing", CheckNewOpeningSupersedesClosing);
    Run("all easing curves and delay", CheckEasingAndDelay);
    Run("invalid starts preserve the running track", CheckInvalidStartsPreserveRunningTrack);
    Run("only the selected direction validates", CheckOnlyTheSelectedDirectionIsValidated);
    Run("deadline saturation preserves Scalar contract",
        CheckDeadlineSaturationMatchesScalarContract);
    Run("counter exhaustion preserves track", CheckCounterExhaustionPreservesTrack);
    Run("borrowed clock and explicit timestamps", CheckBorrowedClockAndExplicitSamples);
}
