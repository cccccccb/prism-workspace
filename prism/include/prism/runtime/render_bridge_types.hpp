#pragma once

#include "prism/contracts/types.hpp"

#include <cstdint>
#include <string>

namespace prism::runtime {

// Values published by the protocol owner after a completed Wayland turn.
// Keep the status behind an ordered event, rather than reading Wayland objects
// from the UI thread. Configure/input events preceding it remain in order.
struct RenderPlatformSnapshot {
    bool close_requested{}, configured{}, mapped{};
    bool frame_callback_pending{}, presentation_feedback{};
    contracts::WindowMetrics metrics{};
    int configure_count{}, frame_done_count{}, presentation_count{};
    std::uint64_t wait_duration_ns{};
    std::uint64_t surface_state_commits{}, surface_pixel_commits{};
    std::uint64_t surface_submission_failures{}, surface_noops{};
};

// Cumulative backend fields of ClientRenderStats. Scene build counts stay on
// the UI owner; platform commit counts stay in RenderPlatformSnapshot.
struct RenderBackendStats {
    std::uint64_t gpu_render_attempts{}, gpu_render_successes{};
    std::uint64_t swap_attempts{}, swap_successes{};
    std::uint64_t image_uploads{}, upload_bytes{}, oversized_uploads{};
    std::uint64_t full_pixel_repairs{}, partial_pixel_repairs{}, empty_pixel_repairs{};
    std::uint64_t pixel_repair_pixels{}, content_damage_pixels{};
    std::uint64_t damage_history_commits{}, buffer_age_queries{}, unknown_buffer_ages{};
    int last_buffer_age{-1};
    bool buffer_age_supported{}, swap_damage_supported{}, partial_update_supported{};
};

// First submit-build timing belongs to the UI owner; the worker reports only
// backend calls performed on its own thread.
struct RenderStartupStats {
    std::uint64_t egl_init_us{}, ganesh_init_us{};
    std::uint64_t first_render_us{}, first_swap_us{};
};

struct RenderStatusEvent {
    RenderPlatformSnapshot platform{};
    RenderBackendStats backend{};
    RenderStartupStats startup{};
    std::string gl_renderer;
    int presented{};
};

} // namespace prism::runtime
