#pragma once

#include "prism/runtime/blueprint.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace prism::runtime {
enum class UiInstallState { Idle, Pending, Committed, Failed, Cancelled };

using BindingValues = std::map<std::string, PropertyValue, std::less<>>;

struct RegionUpdate {
    std::string region;
    Blueprint content;
};

// Cooperative owner-thread limits. A single shaping, layout or driver call
// cannot be preempted; elapsed time is checked between work units.
struct UiInstallLimits {
    std::size_t nodes_per_turn{128};
    std::size_t images_per_turn{2};
    std::uint64_t upload_bytes_per_turn{4 * 1024 * 1024};
    std::chrono::microseconds cpu_per_turn{2000};
};

struct UiInstallStats {
    std::uint64_t turns{}, nodes{}, image_registrations{}, image_uploads{}, upload_bytes{};
    std::uint64_t budget_yields{}, oversized_uploads{}, max_turn_us{};
};
} // namespace prism::runtime
