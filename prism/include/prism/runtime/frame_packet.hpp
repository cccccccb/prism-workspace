#pragma once

#include "prism/contracts/display_list.hpp"
#include "prism/contracts/surface_effect.hpp"
#include "prism/runtime/ui_load.hpp"
#include <cstdint>
#include <memory>
#include <vector>

namespace prism::runtime {

// Immutable after publication through shared_ptr<const FramePacket>. This is
// the first UI-to-render value boundary on the current owner thread. Cross-
// thread resource leases are introduced only when render ownership migrates.
// Damage and buffer age are computed against the last successful submission.
struct FramePacket {
    UiLoadId ui{};
    std::uint64_t pixels_revision{};
    std::uint64_t theme_generation{};
    std::uint64_t resource_epoch{};
    int configure_count{};
    contracts::BufferSize buffer_size{};
    double scale{1.0};
    std::shared_ptr<const contracts::DisplayList> display_list;
    std::vector<contracts::SurfaceEffectRegion> surface_effects;
    std::vector<contracts::SurfaceInputRegion> input_regions;
};

} // namespace prism::runtime
