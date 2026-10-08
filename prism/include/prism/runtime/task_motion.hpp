#pragma once

#include "prism/animation/timeline.hpp"
#include "prism/contracts/motion.hpp"
#include "prism/runtime/task_presentation.hpp"

#include <cstdint>
#include <optional>

namespace prism::runtime {

struct TaskMotionSpec {
    animation::DurationSpec opening;
    animation::DurationSpec closing;
};

// Resolve only task.open/task.close. Each missing entry is instant, including
// the completely empty legacy MotionSet. A nonempty malformed set is rejected
// through the normal motion contract; its profile id never selects a recipe.
TaskMotionSpec ResolveTaskMotionSpec(const contracts::MotionSet &);

struct TaskMotionSample {
    double reveal{};
    // The track's target endpoint. Only Terminal means that endpoint is reached.
    TaskPresentationEndpoint endpoint{TaskPresentationEndpoint::Closed};
    TaskPresentationSampleKind kind{TaskPresentationSampleKind::Terminal};
    std::uint64_t time_ns{};

    bool operator==(const TaskMotionSample &) const noexcept = default;
};

// Prepared on the owner thread and carried by one immutable frame. These
// samples authorize neither input nor business completion by themselves.
struct TaskMotionFrameStamp {
    TaskPresentationIdentity identity;
    std::uint64_t generation{};
    std::uint64_t revision{};
    TaskMotionSample sample;

    bool operator==(const TaskMotionFrameStamp &) const noexcept = default;
};

// Owner-thread normalized trajectory. The borrowed clock must outlive this
// object. It owns no frame, input scope, business task, resource or timer.
// Samples use absolute monotonic time; their cadence never advances motion.
class TaskMotionTimeline {
public:
    explicit TaskMotionTimeline(const animation::AnimationClock &) noexcept;
    TaskMotionTimeline(const TaskMotionTimeline &) = delete;
    TaskMotionTimeline &operator=(const TaskMotionTimeline &) = delete;
    TaskMotionTimeline(TaskMotionTimeline &&) = delete;
    TaskMotionTimeline &operator=(TaskMotionTimeline &&) = delete;

    bool BeginOpen(const TaskMotionSpec &, animation::MonotonicTimeNs now);
    // The caller supplies the last actually adopted reveal in [0,1], rather
    // than a newer unadopted opening sample. Invalid input preserves the track.
    bool BeginClose(const TaskMotionSpec &, double adopted_reveal, animation::MonotonicTimeNs now);
    TaskMotionSample Sample();
    TaskMotionSample SampleAt(animation::MonotonicTimeNs now);
    bool IsActive() const noexcept;
    std::optional<animation::MonotonicTimeNs> CompletionDeadlineNs() const noexcept;

private:
    bool Begin(double from, double target, animation::DurationSpec,
               TaskPresentationEndpoint endpoint, animation::MonotonicTimeNs now);

    const animation::AnimationClock &clock_;
    animation::ScalarTimeline timeline_;
    TaskPresentationEndpoint endpoint_{TaskPresentationEndpoint::Closed};
};

} // namespace prism::runtime
