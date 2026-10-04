#pragma once
#include "prism/contracts/theme.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>
struct wl_display;
struct wlr_renderer;
struct wlr_allocator;
struct wlr_scene;
struct wlr_surface;
struct wlr_scene_node;

namespace prism::wm {
struct WlrXdgView;

// Compositor-only typed backdrop/material pass. Never sees client DSL or Skia.
class SurfaceEffects {
public:
    // Cumulative CPU-side work, not GPU completion or physical memory usage.
    struct WorkCounters {
        std::uint64_t update_calls{}, skipped_updates{}, unsupported_updates{};
        std::uint64_t dirty_transitions{}, wake_notifications{};
        std::uint64_t regions_checked{}, cache_hits{}, cache_misses{};
        // Attempts include failed begins. Passes require successful submission
        // and, for raw GLES draws, no GL error. Allocation clear passes excluded.
        std::uint64_t capture_pass_attempts{}, capture_passes{};
        std::uint64_t blur_pass_attempts{}, blur_passes{};
        std::uint64_t material_pass_attempts{}, material_passes{};
        std::uint64_t allocation_attempts{}, allocated_buffers{}, allocation_failures{};
        // Rendered pixels count each published result's full extent with padding;
        // failed_regions counts one failed region, invalid_regions its size subset.
        std::uint64_t rendered_regions{}, rendered_pixels{}, failed_regions{}, invalid_regions{};
        std::uint64_t removed_regions{}, scene_reorders{};
        // Leaves are counted per dependency evaluation, before/after footprint
        // filtering. Capture nodes count queued draws, including failed passes.
        std::uint64_t dependency_leaves_checked{}, dependency_leaves_included{},
            dependency_leaves_skipped{};
        std::uint64_t content_revisions{}, metadata_commits{}, damage_history_fallbacks{};
        // Successful cache hits whose observations advanced past outside damage.
        std::uint64_t partial_damage_cache_hits{}, capture_nodes{};
    };

    struct UpdateResult {
        bool processed{};     // The current lower-scene dependencies were examined.
        bool scene_changed{}; // A paint was added, removed, repositioned or replaced.
    };

    SurfaceEffects(wl_display *, wlr_renderer *, wlr_allocator *);
    ~SurfaceEffects();
    bool Supported() const;
    // Event-thread-only. The WM must invalidate all lower-scene content, geometry,
    // ordering, visibility, focus and theme changes; callbacks only request a frame.
    void SetWakeHandler(std::function<void()>);
    void MarkDirty();
    // Call after wlroots applies current state, before commit filtering. Damage
    // and mapping are copied here; buffer identity and callback sequence are not
    // content versions. Forget on destruction before a surface address is reused.
    void NotifySurfaceCommit(wlr_surface *);
    void ForgetSurface(wlr_surface *);
    bool NeedsUpdate() const;
    // Borrowed backend paint nodes, ordered below this surface. Event-thread only.
    std::vector<wlr_scene_node *> PresentationNodes(wlr_surface *) const;
    const WorkCounters &Counters() const;
    UpdateResult Update(wlr_scene *, std::span<WlrXdgView *const>, WlrXdgView *focused,
                        const contracts::ThemeSnapshot *theme);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace prism::wm
