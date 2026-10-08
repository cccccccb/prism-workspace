#pragma once

#include "prism/runtime/task_motion.hpp"

namespace prism::sdk {

struct OwnerTaskMotion {
    OwnerTaskMotion(runtime::TaskPresentationIdentity identity, std::uint64_t generation,
                    const animation::AnimationClock &clock, runtime::TaskMotionSpec spec)
        : identity(identity), spec(spec), timeline(clock)
    {
        current.identity = identity;
        current.generation = generation;
        current.revision = 1;
    }

    runtime::TaskPresentationIdentity identity;
    runtime::TaskMotionSpec spec;
    runtime::TaskMotionTimeline timeline;
    runtime::TaskMotionFrameStamp current;
    std::optional<runtime::TaskMotionFrameStamp> adopted;
    bool fallback{};
};

} // namespace prism::sdk
