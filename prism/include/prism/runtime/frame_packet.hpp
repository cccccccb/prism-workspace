#pragma once

#include "prism/contracts/display_list.hpp"
#include "prism/contracts/surface_effect.hpp"
#include "prism/runtime/animation_sample.hpp"
#include "prism/runtime/input_snapshot.hpp"
#include "prism/runtime/popup_surface.hpp"
#include "prism/runtime/render_resource.hpp"
#include "prism/runtime/ui_load.hpp"
#include <cstdint>
#include <memory>
#include <vector>

namespace prism::runtime {

struct PopupSurfaceRequest;
struct PopupFramePacket;

// Immutable after publication through shared_ptr<const FramePacket>. Image
// uses carry UI-assigned versions; ordered resource commands keep decoded
// bytes alive without pinning them in every committed frame. Damage and buffer
// age are computed against the last successful submission.
struct FramePacket {
    UiLoadId ui{};
    std::uint64_t sequence{}; // Nonzero, monotonic within one ClientApplication.
    std::uint64_t scene_revision{};
    std::uint64_t pixels_revision{};
    std::uint64_t theme_generation{};
    std::uint64_t resource_epoch{};
    AnimationSampleStamp animation_sample{};
    int configure_count{};
    contracts::BufferSize buffer_size{};
    double scale{1.0};
    std::shared_ptr<const contracts::DisplayList> display_list;
    std::shared_ptr<const std::vector<ImageVersion>> image_uses;
    std::vector<contracts::SurfaceEffectRegion> surface_effects;
    std::vector<contracts::SurfaceInputRegion> input_regions;
    std::shared_ptr<const InputSnapshot> input_snapshot;
    // Optional Host export intent from the same UI sample. It neither creates
    // a native child nor authorizes input/submission to an existing target.
    // Eligible Popup/Menu leave the root list only after an actual child
    // pixel submission and verified Scene adoption. Unsupported plans stay
    // in the root; metadata adoption cannot replace the first pixel barrier.
    std::shared_ptr<const PopupSurfaceRequest> popup_surface_request;
    std::shared_ptr<const PopupFramePacket> popup_surface_frame;
    // The root list excludes this successfully adopted child. Effects wait for
    // this packet to reach a matching actual root pixel baseline.
    PopupSurfaceIdentity popup_surface_excluded{};
};

} // namespace prism::runtime
