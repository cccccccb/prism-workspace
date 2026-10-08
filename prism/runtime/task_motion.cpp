#include "prism/runtime/task_motion.hpp"

#include <cmath>

namespace prism::runtime {
namespace {
animation::DurationSpec Duration(const contracts::MotionTransition *entry)
{
    if (!entry) {
        return {};
    }

    return {static_cast<std::uint64_t>(entry->duration_ms) * 1'000'000, 0,
            static_cast<animation::Easing>(entry->easing)};
}
} // namespace

TaskMotionSpec ResolveTaskMotionSpec(const contracts::MotionSet &set)
{
    if (set.id.empty() && set.transitions.empty()) {
        return {};
    }

    contracts::ValidateMotion(set);

    return {Duration(contracts::FindMotion(set, "task.open")),
            Duration(contracts::FindMotion(set, "task.close"))};
}

TaskMotionTimeline::TaskMotionTimeline(const animation::AnimationClock &clock) noexcept
    : clock_(clock), timeline_(clock)
{
}

bool TaskMotionTimeline::Begin(double from, double target, animation::DurationSpec spec,
                               TaskPresentationEndpoint endpoint, animation::MonotonicTimeNs now)
{
    const bool accepted = timeline_.State() == animation::MotionState::Idle
                              ? timeline_.StartDurationAt(from, target, spec, now)
                              : timeline_.RestartDurationAt(from, target, spec, now).accepted;
    if (!accepted) {
        return false;
    }

    endpoint_ = endpoint;
    // Instant and already-closed tracks finish immediately. Terminal samples
    // remain classified as Terminal after Scalar's one-shot event is consumed.
    timeline_.SampleAt(now);

    return true;
}

bool TaskMotionTimeline::BeginOpen(const TaskMotionSpec &spec, animation::MonotonicTimeNs now)
{
    return Begin(0, 1, spec.opening, TaskPresentationEndpoint::Open, now);
}

bool TaskMotionTimeline::BeginClose(const TaskMotionSpec &spec, double adopted_reveal,
                                    animation::MonotonicTimeNs now)
{
    if (!std::isfinite(adopted_reveal) || adopted_reveal < 0 || adopted_reveal > 1) {
        return false;
    }

    return Begin(adopted_reveal, 0, spec.closing, TaskPresentationEndpoint::Closed, now);
}

TaskMotionSample TaskMotionTimeline::Sample()
{
    return SampleAt(clock_.NowNs());
}

TaskMotionSample TaskMotionTimeline::SampleAt(animation::MonotonicTimeNs now)
{
    const auto sample = timeline_.SampleAt(now);
    const auto kind = sample.state == animation::MotionState::Running
                          ? TaskPresentationSampleKind::Intermediate
                          : TaskPresentationSampleKind::Terminal;

    return {sample.value, endpoint_, kind, now};
}

bool TaskMotionTimeline::IsActive() const noexcept
{
    return timeline_.State() == animation::MotionState::Running;
}

std::optional<animation::MonotonicTimeNs> TaskMotionTimeline::CompletionDeadlineNs() const noexcept
{
    return timeline_.CompletionDeadlineNs();
}

} // namespace prism::runtime
