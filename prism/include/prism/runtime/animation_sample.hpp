#pragma once

#include <cstdint>

namespace prism::runtime {

// The last visible animation presentation change in one Scene generation.
// revision == 0 means no animated value has ever changed visibly in this
// Scene. A real sample may occur at monotonic time zero, so time_ns alone is
// not a presence flag. This is a sample time, never a packet capture time.
struct AnimationSampleStamp {
    std::uint64_t revision{};
    std::uint64_t time_ns{};

    bool operator==(const AnimationSampleStamp &) const = default;
};

} // namespace prism::runtime
