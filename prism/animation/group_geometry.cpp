#include "prism/animation/group_geometry.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace prism::animation {
namespace {
constexpr std::size_t MaxEdges = 1024, MaxMembers = 256;
constexpr double MaxCoordinate = 10'000'000;

const GroupEdge *Find(const std::vector<GroupEdge> &edges, std::uint64_t id)
{
    const auto it =
        std::lower_bound(edges.begin(), edges.end(), id,
                         [](const GroupEdge &edge, auto value) { return edge.id < value; });
    return it != edges.end() && it->id == id ? &*it : nullptr;
}

bool Normalize(GroupGeometryLayout &layout)
{
    if (layout.edges.empty() || layout.edges.size() > MaxEdges || layout.members.empty() ||
        layout.members.size() > MaxMembers) {
        return false;
    }

    std::sort(layout.edges.begin(), layout.edges.end(),
              [](const auto &a, const auto &b) { return a.id < b.id; });
    std::sort(layout.members.begin(), layout.members.end(),
              [](const auto &a, const auto &b) { return a.id < b.id; });
    std::uint64_t previous{};
    for (auto &edge : layout.edges) {
        if (!edge.id || edge.id == previous || !std::isfinite(edge.position) ||
            std::abs(edge.position) > MaxCoordinate ||
            (edge.axis != GeometryAxis::X && edge.axis != GeometryAxis::Y)) {
            return false;
        }
        previous = edge.id;
        edge.position = std::round(edge.position);
    }

    previous = 0;
    for (const auto &member : layout.members) {
        if (!member.id || member.id == previous) {
            return false;
        }
        previous = member.id;
        const auto *left = Find(layout.edges, member.left.id);
        const auto *right = Find(layout.edges, member.right.id);
        const auto *top = Find(layout.edges, member.top.id);
        const auto *bottom = Find(layout.edges, member.bottom.id);
        if (!left || !right || !top || !bottom || left->axis != GeometryAxis::X ||
            right->axis != GeometryAxis::X || top->axis != GeometryAxis::Y ||
            bottom->axis != GeometryAxis::Y) {
            return false;
        }
        for (auto ref : {member.left, member.top, member.right, member.bottom}) {
            if (std::abs(double(ref.offset)) > MaxCoordinate) {
                return false;
            }
        }
        if (left->position + member.left.offset >= right->position + member.right.offset ||
            top->position + member.top.offset >= bottom->position + member.bottom.offset) {
            return false;
        }
    }
    return true;
}

bool SameTopology(const GroupGeometryLayout &a, const GroupGeometryLayout &b)
{
    if (a.members != b.members || a.edges.size() != b.edges.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.edges.size(); ++i) {
        if (a.edges[i].id != b.edges[i].id || a.edges[i].axis != b.edges[i].axis) {
            return false;
        }
    }
    return true;
}
} // namespace

GroupGeometryTimeline::GroupGeometryTimeline(const AnimationClock &clock) noexcept
    : progress_(clock)
{
}

GroupGeometryFrame GroupGeometryTimeline::Frame(std::vector<GroupEdge> edges, MotionState state)
{
    if (serial_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Group geometry serial exhausted");
    }
    GroupGeometryFrame frame{++serial_, progress_.Generation(), state, std::move(edges), {}};
    frame.windows.reserve(target_.members.size());
    for (const auto &member : target_.members) {
        const double left = Find(frame.edges, member.left.id)->position + member.left.offset;
        const double top = Find(frame.edges, member.top.id)->position + member.top.offset;
        const double right = Find(frame.edges, member.right.id)->position + member.right.offset;
        const double bottom = Find(frame.edges, member.bottom.id)->position + member.bottom.offset;
        frame.windows.push_back({member.id, {left, top, right - left, bottom - top}});
    }
    return frame;
}

bool GroupGeometryTimeline::Reset(GroupGeometryLayout layout)
{
    if (!Normalize(layout)) {
        return false;
    }
    const bool accepted = progress_.State() == MotionState::Idle
                              ? progress_.StartDuration(1, 1, {})
                              : progress_.RestartDuration(1, 1, {}).accepted;
    if (!accepted) {
        return false;
    }
    progress_.Sample();

    target_ = std::move(layout);
    from_ = target_.edges;
    submitted_ = Frame(from_, MotionState::Finished);
    candidate_.reset();
    pending_ = false;
    return true;
}

bool GroupGeometryTimeline::Retarget(GroupGeometryLayout target, DurationSpec spec,
                                     MonotonicTimeNs now)
{
    if (!submitted_.serial || !Normalize(target) || !SameTopology(target_, target)) {
        return false;
    }
    if (!progress_.RestartDurationAt(0, 1, spec, now).accepted) {
        return false;
    }

    from_ = submitted_.edges;
    target_ = std::move(target);
    candidate_.reset();
    pending_ = true;
    return true;
}

GroupGeometryFrame GroupGeometryTimeline::Prepare(MonotonicTimeNs now)
{
    if (!submitted_.serial) {
        throw std::logic_error("Group geometry requires a submitted baseline");
    }
    const auto sample = progress_.SampleAt(now);
    auto edges = target_.edges;
    for (std::size_t i = 0; i < edges.size(); ++i) {
        edges[i].position =
            std::round(std::lerp(from_[i].position, edges[i].position, sample.value));
    }

    candidate_ = Frame(std::move(edges), sample.state);
    return *candidate_;
}

bool GroupGeometryTimeline::Accept(std::uint64_t serial)
{
    if (!candidate_ || candidate_->serial != serial) {
        return false;
    }
    submitted_ = std::move(*candidate_);
    candidate_.reset();
    pending_ = submitted_.state == MotionState::Running;
    return true;
}

const GroupGeometryFrame &GroupGeometryTimeline::Submitted() const noexcept
{
    return submitted_;
}

bool GroupGeometryTimeline::NeedsFrame() const noexcept
{
    return pending_;
}
} // namespace prism::animation
