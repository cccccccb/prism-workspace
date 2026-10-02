#pragma once

#include "prism/contracts/launch.hpp"
#include "prism/contracts/types.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace prism::contracts {

inline constexpr std::uint16_t kLayoutSnapshotVersion = 1;
inline constexpr std::size_t kMaxLayoutSnapshotPayload = 192 * 1024;
inline constexpr std::size_t kMaxLayoutOutputs = 16;
inline constexpr std::size_t kMaxLayoutWorkspaces = 64;
inline constexpr std::size_t kMaxLayoutNodes = 512;
inline constexpr std::size_t kMaxLayoutBoundaries = 512;

// WM identities are nonzero, monotonic and never reused within one session.
// They are independent of client Scene NodeId and of display names.
enum class LayoutNodeKind : std::uint8_t { Container, View };
enum class LayoutArrangement : std::uint8_t { None, Horizontal, Vertical, Tabbed, Stacked };
enum class LayoutBoundaryAxis : std::uint8_t { X, Y };
enum class LayoutGroupMode : std::uint8_t { Normal, Immersive };

struct LayoutOutput {
    std::uint64_t id{};
    std::string name;
    LogicalRect logical_bounds;
    double scale{1.0};
    bool primary{};
    bool supported{};
    bool operator==(const LayoutOutput &) const = default;
};

struct LayoutWorkspace {
    std::uint64_t id{};
    std::uint64_t root{};
    std::uint64_t output{};
    std::string name;
    bool active{};
    LayoutGroupMode mode{LayoutGroupMode::Normal};
    std::uint64_t mode_revision{1};
    bool operator==(const LayoutWorkspace &) const = default;
};

struct LayoutNode {
    std::uint64_t id{};
    std::uint64_t parent{};
    std::uint64_t workspace{};
    LayoutNodeKind kind{LayoutNodeKind::Container};
    LayoutArrangement layout{LayoutArrangement::None};
    std::vector<std::uint64_t> children;
    LogicalRect tile_bounds;
    LogicalRect target_bounds;
    // Client-committed geometry observed by the WM, not output presentation.
    LogicalRect committed_bounds;
    double width_fraction{};
    double height_fraction{};
    InstanceId instance;
    bool focused{};
    bool visible{};
    bool fullscreen{};
    bool has_committed{};
    bool operator==(const LayoutNode &) const = default;
};

struct LayoutBoundary {
    std::uint64_t id{};
    std::uint64_t parent{};
    std::uint64_t first{};
    std::uint64_t second{};
    std::uint64_t workspace{};
    LayoutBoundaryAxis axis{LayoutBoundaryAxis::X};
    LogicalRect bounds;
    bool visible{};
    // Always false in version 1: constraints and control sessions come later.
    bool resizable{};
    bool operator==(const LayoutBoundary &) const = default;
};

struct LayoutSnapshot {
    std::uint64_t session{};
    std::uint64_t revision{};
    std::uint64_t topology_revision{};
    std::uint64_t layout_revision{};
    std::uint64_t focus_revision{};
    InstanceId active_instance;
    std::vector<LayoutOutput> outputs;
    std::vector<LayoutWorkspace> workspaces;
    std::vector<LayoutNode> nodes;
    std::vector<LayoutBoundary> boundaries;
    bool operator==(const LayoutSnapshot &) const = default;
};

// Display labels do not carry identity. Replace malformed UTF-8/control code points,
// preserve complete code points up to 128 bytes, and use "Unnamed" if empty.
std::string LayoutDisplayName(std::string_view text);

// Throws invalid_argument for malformed, oversized or inconsistent values.
void ValidateLayoutSnapshot(const LayoutSnapshot &snapshot);
std::vector<std::uint8_t> EncodeLayoutSnapshot(const LayoutSnapshot &snapshot);
LayoutSnapshot DecodeLayoutSnapshot(std::span<const std::uint8_t> bytes);

} // namespace prism::contracts
