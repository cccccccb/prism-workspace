#pragma once
#include "prism/animation/timeline.hpp"
#include "prism/contracts/types.hpp"
#include <optional>
#include <vector>

namespace prism::animation {
enum class GeometryAxis { X, Y };

struct GroupEdge {
    std::uint64_t id{};
    GeometryAxis axis{};
    double position{};
    bool operator==(const GroupEdge &) const = default;
};

// Offset is a fixed logical-pixel inset from a shared boundary, e.g. half a gap.
struct GroupEdgeRef {
    std::uint64_t id{};
    int offset{};
    bool operator==(const GroupEdgeRef &) const = default;
};

struct GroupMember {
    std::uint64_t id{};
    GroupEdgeRef left, top, right, bottom;
    bool operator==(const GroupMember &) const = default;
};

struct GroupGeometryLayout {
    std::vector<GroupEdge> edges;
    std::vector<GroupMember> members;
};

struct GroupWindowBounds {
    std::uint64_t id{};
    contracts::LogicalRect bounds;
};

struct GroupGeometryFrame {
    std::uint64_t serial{}, generation{};
    MotionState state{MotionState::Idle};
    std::vector<GroupEdge> edges;
    std::vector<GroupWindowBounds> windows;
};

// Owner-thread, bounded coordinator; no BSP mutation, output or client calls.
// A shared edge is sampled and rounded once for the entire group.
class GroupGeometryTimeline {
public:
    explicit GroupGeometryTimeline(const AnimationClock &clock) noexcept;
    // Reset asserts that this layout is already the owner's visible baseline.
    bool Reset(GroupGeometryLayout layout);
    // Same identities, references, axes and fixed gaps only. Other changes require Reset.
    // Reversal starts from the last accepted frame, never an unsubmitted candidate.
    bool Retarget(GroupGeometryLayout target, DurationSpec spec, MonotonicTimeNs now);
    GroupGeometryFrame Prepare(MonotonicTimeNs now);
    // Call only after the whole output transaction succeeds. Stale serials fail.
    bool Accept(std::uint64_t serial);
    const GroupGeometryFrame &Submitted() const noexcept;
    bool NeedsFrame() const noexcept;

private:
    GroupGeometryFrame Frame(std::vector<GroupEdge> edges, MotionState state);
    ScalarTimeline progress_;
    GroupGeometryLayout target_;
    std::vector<GroupEdge> from_;
    GroupGeometryFrame submitted_;
    std::optional<GroupGeometryFrame> candidate_;
    std::uint64_t serial_{};
    bool pending_{};
};
} // namespace prism::animation
