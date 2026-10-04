#include "../prism/src/wm/wlr_group_geometry.hpp"
#include <cassert>
using namespace prism;

namespace {
class Clock final : public animation::AnimationClock {
public:
    animation::MonotonicTimeNs NowNs() const noexcept override
    {
        return 0;
    }
};

contracts::LayoutSnapshot Snapshot(double height)
{
    contracts::LayoutSnapshot snapshot;
    contracts::LayoutOutput output;
    output.id = 1;
    output.supported = true;
    snapshot.outputs.push_back(output);
    contracts::LayoutWorkspace workspace;
    workspace.id = 1;
    workspace.root = 10;
    workspace.active = true;
    snapshot.workspaces.push_back(workspace);
    for (auto id : {10, 11, 20, 21, 22}) {
        contracts::LayoutNode node;
        node.id = id;
        node.workspace = 1;
        node.visible = node.has_committed = true;
        node.kind = id == 10 || id == 20 ? contracts::LayoutNodeKind::Container
                                         : contracts::LayoutNodeKind::View;
        snapshot.nodes.push_back(node);
    }
    auto &nodes = snapshot.nodes;
    nodes[0].layout = contracts::LayoutArrangement::Horizontal;
    nodes[0].children = {11, 20};
    nodes[0].tile_bounds = {0, 0, 1000, height};
    nodes[1].tile_bounds = {0, 0, 492, height};
    nodes[2].layout = contracts::LayoutArrangement::Vertical;
    nodes[2].children = {21, 22};
    nodes[2].tile_bounds = {500, 0, 500, height};
    nodes[3].tile_bounds = {500, 0, 500, height / 2 - 4};
    nodes[4].tile_bounds = {500, height / 2 + 4, 500, height / 2 - 4};
    for (auto &node : nodes) {
        node.target_bounds = node.committed_bounds = node.tile_bounds;
    }
    return snapshot;
}
} // namespace

int main()
{
    auto before = Snapshot(600), after = Snapshot(800);
    const auto from = wm::BuildGroupGeometry(before), target = wm::BuildGroupGeometry(after);
    assert(from && target && from->members.size() == 3 && from->edges.size() == 6);
    // The right subtree inherits the actual parent split, not a coincident coordinate ID.
    assert(from->members[0].right.id == from->members[1].left.id);
    assert(from->members[1].left.id == from->members[2].left.id);
    assert(from->members[1].bottom.id == from->members[2].top.id);
    Clock clock;
    animation::GroupGeometryTimeline timeline(clock);
    assert(timeline.Reset(*from));
    assert(timeline.Retarget(*target, {100}, 0));
    const auto frame = timeline.Prepare(37);
    const auto &a = frame.windows[0].bounds;
    const auto &b = frame.windows[1].bounds;
    const auto &c = frame.windows[2].bounds;
    assert(b.x - a.x - a.width == 8 && c.y - b.y - b.height == 8);
    assert(c.y + c.height == a.y + a.height);
    assert(timeline.Accept(frame.serial));
    const auto final = timeline.Prepare(100);
    assert(final.windows[0].bounds == after.nodes[1].target_bounds);
    assert(final.windows[1].bounds == after.nodes[3].target_bounds);
    assert(final.windows[2].bounds == after.nodes[4].target_bounds);

    after.nodes[2].layout = contracts::LayoutArrangement::Tabbed;
    assert(!wm::BuildGroupGeometry(after));
    after = Snapshot(800);
    after.nodes[1].fullscreen = true;
    assert(!wm::BuildGroupGeometry(after));
    after = Snapshot(800);
    after.nodes[1].visible = false;
    assert(!wm::BuildGroupGeometry(after));
    after = Snapshot(800);
    after.outputs.push_back(after.outputs.front());
    assert(!wm::BuildGroupGeometry(after));
}
