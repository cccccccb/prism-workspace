#include "prism/animation/group_geometry.hpp"
#include <algorithm>
#include <cassert>
#include <limits>
using namespace prism::animation;

namespace {
class Clock final : public AnimationClock {
public:
    MonotonicTimeNs NowNs() const noexcept override
    {
        return 0;
    }
};

GroupGeometryLayout Layout(double split = 400)
{
    // Left pane and two right panes share a T junction. A deliberate 8 px gap
    // is represented by fixed offsets, not two independently animated edges.
    return {{{1, GeometryAxis::X, -100},
             {2, GeometryAxis::X, split},
             {3, GeometryAxis::X, 900},
             {4, GeometryAxis::Y, 20},
             {5, GeometryAxis::Y, 301},
             {6, GeometryAxis::Y, 620}},
            {{11, {1, 0}, {4, 0}, {2, -4}, {6, 0}},
             {12, {2, 4}, {4, 0}, {3, 0}, {5, 0}},
             {13, {2, 4}, {5, 0}, {3, 0}, {6, 0}}}};
}

void CheckJunction(const GroupGeometryFrame &frame)
{
    const auto &a = frame.windows[0].bounds;
    const auto &b = frame.windows[1].bounds;
    const auto &c = frame.windows[2].bounds;
    assert(a.width > 0 && b.width > 0 && c.width > 0);
    assert(a.height > 0 && b.height > 0 && c.height > 0);
    assert(b.x - (a.x + a.width) == 8);
    assert(c.x == b.x && b.y + b.height == c.y);
    assert(c.y + c.height == a.y + a.height);
}
} // namespace

int main()
{
    Clock clock;
    GroupGeometryTimeline group(clock);
    assert(!group.Retarget(Layout(), {100}, 0));
    assert(group.Reset(Layout()));
    const auto initial = group.Submitted();
    assert(!group.NeedsFrame());
    auto target = Layout(650);
    target.edges[4].position = 201;
    assert(group.Retarget(target, {100}, 1000));
    assert(group.NeedsFrame());

    // All edges use one sample regardless of node order or irregular frame times.
    for (auto time : {1000, 1003, 1017, 1041, 1079, 1100}) {
        auto frame = group.Prepare(time);
        CheckJunction(frame);
        assert(group.Accept(frame.serial));
    }
    assert(!group.NeedsFrame());
    assert(group.Submitted().edges == target.edges);
    assert(!group.Accept(initial.serial));

    // A failed submit must retain the complete old group. Reversal starts there.
    assert(group.Retarget(Layout(), {100}, 1200));
    const auto shown = group.Prepare(1240);
    assert(group.Accept(shown.serial));
    const auto failed = group.Prepare(1270);
    assert(group.Submitted().edges == shown.edges);
    assert(group.Retarget(target, {100}, 1270));
    assert(!group.Accept(failed.serial));
    const auto reverse = group.Prepare(1270);
    assert(reverse.edges == shown.edges && reverse.generation > shown.generation);
    assert(group.Accept(reverse.serial));
    const auto late = group.Prepare(2000);
    assert(late.edges == target.edges && group.NeedsFrame());
    assert(group.Accept(late.serial) && !group.NeedsFrame());

    // New candidate replaces old one; zero duration still needs one successful submit.
    assert(group.Retarget(Layout(), {0}, 3000));
    const auto stale = group.Prepare(3000);
    const auto current = group.Prepare(3001);
    assert(!group.Accept(stale.serial));
    assert(group.Accept(current.serial) && !group.NeedsFrame());
    assert(group.Submitted().edges == initial.edges);

    // Canonical ordering is independent of caller iteration order.
    std::reverse(target.edges.begin(), target.edges.end());
    std::reverse(target.members.begin(), target.members.end());
    assert(group.Retarget(target, {100, 20, Easing::EaseInOutCubic}, 4000));
    const auto delayed = group.Prepare(4010);
    assert(delayed.edges == initial.edges);
    assert(group.Accept(delayed.serial));
    for (int i = 20; i <= 120; ++i) {
        const auto frame = group.Prepare(4000 + i);
        CheckJunction(frame);
        assert(group.Accept(frame.serial));
    }

    // Invalid topology, coordinates and duration fail atomically.
    const auto baseline = group.Submitted();
    auto bad = Layout();
    bad.edges[1].position = std::numeric_limits<double>::quiet_NaN();
    assert(!group.Retarget(bad, {100}, 5000));
    bad = Layout();
    bad.members[1].left.id = 99;
    assert(!group.Reset(bad));
    bad = Layout();
    bad.edges[1].id = 1;
    assert(!group.Reset(bad));
    bad = Layout();
    bad.members[1].id = 11;
    assert(!group.Reset(bad));
    bad = Layout();
    bad.edges[0].axis = GeometryAxis::Y;
    assert(!group.Reset(bad));
    bad = Layout();
    bad.members[0].right.offset = -1000;
    assert(!group.Reset(bad));
    bad = Layout();
    bad.members[0].right.offset = -3;
    assert(!group.Retarget(bad, {100}, 5000));
    bad = Layout();
    bad.edges.resize(1025);
    assert(!group.Reset(bad));
    assert(!group.Retarget(Layout(), {100, 0, static_cast<Easing>(99)}, 5000));
    assert(group.Submitted().serial == baseline.serial && !group.NeedsFrame());

    // A rejected request preserves an in-flight candidate and its acknowledgement.
    assert(group.Retarget(Layout(), {100}, 4500));
    const auto retained = group.Prepare(4540);
    bad = Layout();
    bad.members.pop_back();
    assert(!group.Retarget(bad, {100}, 4550));
    assert(group.Accept(retained.serial));
    assert(group.Submitted().edges == retained.edges);

    // Sparse and dense sampling at the same timestamp must produce identical edges.
    GroupGeometryTimeline sparse(clock), dense(clock);
    assert(sparse.Reset(Layout()) && dense.Reset(Layout()));
    assert(sparse.Retarget(Layout(600), {100}, 6000));
    assert(dense.Retarget(Layout(600), {100}, 6000));
    for (int i = 0; i < 79; ++i) {
        const auto frame = dense.Prepare(6000 + i);
        assert(dense.Accept(frame.serial));
    }
    assert(sparse.Prepare(6079).edges == dense.Prepare(6079).edges);

    assert(group.Retarget(Layout(), {100}, 5000));
    const auto cancelled = group.Prepare(5020);
    assert(group.Reset(Layout(300)));
    assert(!group.Accept(cancelled.serial) && !group.NeedsFrame());
}
