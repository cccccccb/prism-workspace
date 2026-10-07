#pragma once

#include "prism/contracts/display_list.hpp"
#include "prism/contracts/popup_positioner.hpp"
#include "prism/contracts/surface_effect.hpp"
#include "prism/runtime/input_snapshot.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace prism::runtime {
struct PopupSurfaceSource;
struct PopupSurfacePrepared;

// Assigned by the Host after a successful, target-scoped pixel commit.
// A local input descriptor alone never authorizes Scene input adoption.
struct PopupSurfaceIdentity {
    std::uint64_t worker{}, target{}, lifetime{}, configure_generation{}, submission_sequence{};
    bool operator==(const PopupSurfaceIdentity &) const = default;
};

struct PopupSurfacePreparationStats {
    std::uint64_t preparations{}, layouts{}, layout_reuses{};
    bool operator==(const PopupSurfacePreparationStats &) const = default;
};

// One active logical replacement panel, with the mapped root as native parent.
// The immutable source retains prepared values, never a live Scene or DSL text.
struct PopupSurfaceRequest {
    std::uint64_t scene{}, popup_token{};
    contracts::NodeId active_node{}, trigger{};
    std::uint64_t parent_configure_generation{};
    std::uint64_t scene_revision{}, pixels_revision{}, theme_generation{};
    bool requires_backdrop{};
    contracts::LogicalRect parent_window_geometry{};
    contracts::LogicalRect anchor{}; // Parent surface coordinates.
    contracts::LogicalSize desired_body{};
    contracts::LogicalSize desired_geometry{}; // Body plus functional attachment; excludes shadow.
    double gap{8};
    contracts::PopupHorizontalAlignment horizontal_alignment{
        contracts::PopupHorizontalAlignment::Center};
    contracts::PopupVerticalPreference vertical_preference{
        contracts::PopupVerticalPreference::Below};
    std::shared_ptr<const PopupSurfaceSource> source;

    bool operator==(const PopupSurfaceRequest &) const = default;
};

struct PopupSurfaceConfigure {
    std::uint64_t parent_configure_generation{}, configure_generation{};
    // Authoritative integer window bounds, including the functional attachment,
    // relative to parent window geometry. Decorative shadow is outside them.
    contracts::LogicalRect window_bounds{};
};

// Input geometry remains descriptive and inadmissible to root Scene input.
// Only Host-confirmed adoption of the owning plan opens the child input gate.
struct PopupSurfacePlan {
    PopupSurfaceRequest request;
    std::uint64_t configure_generation{};
    contracts::LogicalRect body_bounds{};     // Owner/parent surface coordinates.
    contracts::LogicalPoint surface_origin{}; // Surface (0,0) in owner coordinates.
    contracts::BufferSize buffer_size{};
    contracts::LogicalRect window_geometry{}; // Body plus neck in child surface coordinates.
    contracts::LogicalRect body_geometry{};   // Logical body in child surface coordinates.
    contracts::ResourceId font{};
    std::shared_ptr<const contracts::DisplayList> display_list;
    std::shared_ptr<const InputSnapshot> input_snapshot;
    std::vector<contracts::SurfaceInputRegion> input_regions;
    // Exact final-configure clips in child buffer-local logical coordinates.
    // Sampling sources and native transport remain owned by Host/WM.
    std::vector<contracts::SurfaceEffectRegion> effect_regions;
    std::shared_ptr<const PopupSurfacePrepared> prepared;
};
} // namespace prism::runtime
