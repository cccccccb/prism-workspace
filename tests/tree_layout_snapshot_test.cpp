#include "prism/contracts/layout_snapshot.hpp"
#include "prism/tree/tree_engine.hpp"
#include "prism/wm/window.hpp"
#include <cassert>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>

using namespace prism;
using namespace prism::tree;

namespace {

std::shared_ptr<wm::Window> Window(const char *name)
{
    return std::make_shared<wm::Window>(name, name, core::Rect{}, nullptr);
}

void Arrange(TreeEngine &engine)
{
    engine.Arrange({0, 0, 1200, 800}, {12, 16, false, 28});
}

const TreeNodeSnapshot &Node(const TreeSnapshot &snapshot, std::uint64_t id)
{
    const auto found = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
                                    [id](const TreeNodeSnapshot &node) { return node.id == id; });
    assert(found != snapshot.nodes.end());
    return *found;
}

std::uint64_t Boundary(const TreeSnapshot &snapshot, std::uint64_t before, std::uint64_t after)
{
    const auto found = std::find_if(snapshot.boundaries.begin(), snapshot.boundaries.end(),
                                    [before, after](const TreeBoundarySnapshot &boundary) {
                                        return boundary.before == before && boundary.after == after;
                                    });
    return found == snapshot.boundaries.end() ? 0 : found->id;
}

void IdentityAndNoOp()
{
    TreeEngine engine;
    const auto first = engine.InsertWindow(Window("a"));
    const auto second = engine.InsertWindow(Window("b"));
    const auto third = engine.InsertWindow(Window("c"));
    Arrange(engine);
    const auto initial = engine.CaptureSnapshot();
    assert(initial->boundaries.size() == 2);
    assert(initial->active_workspace == first->GetWorkspace()->GetNodeId());
    assert(Node(*initial, first->GetNodeId()).parent == first->GetParent()->GetNodeId());
    assert(Node(*initial, third->GetNodeId()).workspace == initial->active_workspace);
    assert(initial->focused_node == third->GetNodeId());
    const auto root = first->GetParent();
    assert(Node(*initial, root->GetNodeId()).children.size() == 3);
    for (const auto &edge : initial->boundaries) {
        assert(edge.axis == LayoutMode::SplitHorizontal);
        assert(edge.bounds.width == 12);
        assert(edge.bounds.height > 0);
    }

    for (int i = 0; i < 100; ++i) {
        Arrange(engine);
        engine.SetFocus(third);
        engine.SetLayoutMode(root, LayoutMode::SplitHorizontal);
        assert(engine.CaptureSnapshot() == initial);
    }
    engine.SetFocus(first);
    const auto focused = engine.CaptureSnapshot();
    assert(focused->focus_revision > initial->focus_revision);
    assert(focused->topology_revision == initial->topology_revision);
    assert(focused->layout_revision == initial->layout_revision);

    const auto first_id = first->GetNodeId();
    assert(engine.RemoveWindow(first->GetWindow()));
    const auto replacement = engine.InsertWindow(Window("replacement"));
    assert(replacement->GetNodeId() != first_id);
    assert(replacement->GetNodeId() != second->GetNodeId());
    const auto json = nlohmann::json::parse(replacement->ToJson());
    assert(json.at("id").get<std::uint64_t>() == replacement->GetNodeId());
    assert(nlohmann::json::parse(engine.DumpTreeJson()).at("focused_id") ==
           replacement->GetNodeId());
    assert(initial->nodes.size() == 5); // Owned old value remains unchanged after mutation.
}

void AdjacencyLifetime()
{
    TreeEngine engine;
    const auto a = engine.InsertWindow(Window("a"));
    const auto b = engine.InsertWindow(Window("b"));
    const auto c = engine.InsertWindow(Window("c"));
    const auto d = engine.InsertWindow(Window("d"));
    Arrange(engine);
    const auto old = engine.CaptureSnapshot();
    const auto ab = Boundary(*old, a->GetNodeId(), b->GetNodeId());
    const auto cd = Boundary(*old, c->GetNodeId(), d->GetNodeId());
    assert(ab != 0 && cd != 0);

    // Both changes happen between publications: an edge's disappearance cannot be hidden.
    assert(engine.SwapNodes(a, b));
    assert(engine.SwapNodes(a, b));
    const auto reordered = engine.CaptureSnapshot();
    assert(Boundary(*reordered, a->GetNodeId(), b->GetNodeId()) != ab);
    assert(Boundary(*reordered, c->GetNodeId(), d->GetNodeId()) == cd);
    assert(reordered->topology_revision > old->topology_revision);

    assert(engine.RemoveWindow(b->GetWindow()));
    const auto removed = engine.CaptureSnapshot();
    assert(Boundary(*removed, a->GetNodeId(), b->GetNodeId()) == 0);
    assert(Boundary(*removed, a->GetNodeId(), c->GetNodeId()) != 0);
    assert(Boundary(*removed, c->GetNodeId(), d->GetNodeId()) == cd);

    const auto root = a->GetParent();
    engine.SetLayoutMode(root, LayoutMode::Tabbed);
    assert(engine.CaptureSnapshot()->boundaries.empty());
    engine.SetLayoutMode(root, LayoutMode::SplitHorizontal);
    assert(Boundary(*engine.CaptureSnapshot(), c->GetNodeId(), d->GetNodeId()) != cd);
    engine.SetLayoutMode(root, LayoutMode::SplitVertical);
    Arrange(engine);
    for (const auto &edge : engine.CaptureSnapshot()->boundaries) {
        assert(edge.axis == LayoutMode::SplitVertical && edge.bounds.height == 12);
    }
}

void FractionsAndNestedPruning()
{
    TreeEngine engine;
    const auto a = engine.InsertWindow(Window("a"));
    const auto b = engine.InsertWindow(Window("b"));
    const auto c = engine.InsertWindow(Window("c"), Direction::Down, b);
    Arrange(engine);
    const auto old = engine.CaptureSnapshot();
    assert(old->boundaries.size() == 2);
    const auto nested = b->GetParent();
    assert(nested != a->GetParent());
    assert(Node(*old, nested->GetNodeId()).layout == LayoutMode::SplitVertical);

    assert(b->SetFractions(b->GetWidthFraction(), .25));
    assert(c->SetFractions(c->GetWidthFraction(), .75));
    Arrange(engine);
    const auto resized = engine.CaptureSnapshot();
    assert(resized->topology_revision == old->topology_revision);
    assert(resized->layout_revision > old->layout_revision);
    assert(Boundary(*resized, b->GetNodeId(), c->GetNodeId()) ==
           Boundary(*old, b->GetNodeId(), c->GetNodeId()));
    assert(b->GetBounds().height < c->GetBounds().height);
    assert(!b->SetFractions(std::numeric_limits<double>::quiet_NaN(), .5));
    assert(!b->SetFractions(-1, .5));
    assert(!b->SetFractions(std::numeric_limits<double>::infinity(), .5));
    assert(engine.CaptureSnapshot() == resized);

    // Detaching an entire subtree revokes its internal boundary identities too.
    const auto nested_parent = nested->GetParent();
    const auto old_boundary = Boundary(*resized, b->GetNodeId(), c->GetNodeId());
    assert(nested_parent->RemoveChild(nested));
    nested_parent->AddChild(nested);
    const auto reattached = engine.CaptureSnapshot();
    assert(Boundary(*reattached, b->GetNodeId(), c->GetNodeId()) != old_boundary);

    // Ancestor swaps would create cycles; no revision or ownership changes are allowed.
    assert(!engine.SwapNodes(nested, b));
    assert(engine.CaptureSnapshot() == reattached);
    assert(engine.RemoveWindow(c->GetWindow()));
    Arrange(engine);
    const auto pruned = engine.CaptureSnapshot();
    assert(b->GetParent() == a->GetParent());
    assert(pruned->boundaries.size() == 1);
    assert(Boundary(*pruned, a->GetNodeId(), b->GetNodeId()) != 0);
    assert(pruned->topology_revision > resized->topology_revision);
}

void GroupSplitAndCrossSwap()
{
    TreeEngine engine;
    const auto a = engine.InsertWindow(Window("a"));
    const auto b = engine.InsertWindow(Window("b"));
    const auto c = engine.InsertWindow(Window("c"), Direction::Down, b);
    Arrange(engine);
    const auto original = engine.CaptureSnapshot();
    const auto nested = b->GetParent();

    assert(engine.SwapNodes(a, b));
    Arrange(engine);
    const auto swapped = engine.CaptureSnapshot();
    assert(a->GetParent() == nested && b->GetParent() != nested);
    assert(swapped->topology_revision > original->topology_revision);
    assert(Boundary(*swapped, a->GetNodeId(), c->GetNodeId()) != 0);
    assert(Node(*swapped, a->GetNodeId()).parent == nested->GetNodeId());

    engine.SetFocus(b);
    assert(engine.SplitFocused(LayoutMode::SplitVertical));
    const auto split = engine.CaptureSnapshot();
    assert(split->topology_revision > swapped->topology_revision);
    assert(Node(*split, b->GetParent()->GetNodeId()).children.size() == 1);
    const auto tabbed = engine.GroupTabbed(b, Window("tab"));
    assert(tabbed && tabbed->GetChildren().size() == 2);
    const auto grouped = engine.CaptureSnapshot();
    assert(grouped->topology_revision > split->topology_revision);
    assert(Node(*grouped, tabbed->GetNodeId()).layout == LayoutMode::Tabbed);
    assert(Node(*grouped, tabbed->GetNodeId()).active_child == grouped->focused_node);

    engine.GroupTabbed(tabbed, Window("another tab"));
    const auto appended = engine.CaptureSnapshot();
    assert(Node(*appended, tabbed->GetNodeId()).children.size() == 3);
    assert(appended->topology_revision > grouped->topology_revision);
    assert(engine.RemoveWindow(c->GetWindow()));
    assert(a->GetParent() != nested);
    assert(engine.CaptureSnapshot()->topology_revision > appended->topology_revision);
}

void ActiveIdentityAcrossMutations()
{
    TreeEngine engine;
    const auto a = engine.InsertWindow(Window("a"));
    const auto b = engine.InsertWindow(Window("b"));
    const auto c = engine.InsertWindow(Window("c"));
    const auto root = engine.GetActiveWorkspace()->GetRootContainer();
    engine.SetLayoutMode(root, LayoutMode::Tabbed);
    assert(root->GetActiveChild() == c);

    // Removing an earlier inactive sibling preserves the selected tab identity.
    assert(engine.RemoveWindow(a->GetWindow()));
    assert(root->GetActiveChild() == c);
    assert(root->GetActiveChildIndex() == 1);
    assert(engine.CaptureSnapshot()->focused_node == c->GetNodeId());
    assert(engine.SwapNodes(b, c));
    assert(root->GetActiveChild() == c && root->GetActiveChildIndex() == 0);

    assert(engine.MoveWindowToWorkspace(b->GetWindow(), "2"));
    assert(engine.SwitchWorkspace("2"));
    assert(engine.GetFocusedNode() == b);
    assert(engine.SwapNodes(b, c));
    assert(engine.GetFocusedNode() == c);
    assert(c->GetWindow()->IsFocused());
    assert(!b->GetWindow()->IsFocused());
    assert(engine.CaptureSnapshot()->focused_node == c->GetNodeId());

    // Workspace 1's saved node moved away: restoration must select its surviving view.
    assert(engine.SwitchWorkspace("1"));
    assert(engine.GetFocusedNode() == b);
    assert(b->GetWindow()->IsFocused());
    assert(!c->GetWindow()->IsFocused());
    assert(engine.CaptureSnapshot()->focused_node == b->GetNodeId());
}

void AliasedChildArguments()
{
    const auto container = std::make_shared<ContainerNode>();
    const auto a = std::make_shared<ViewNode>(Window("a"));
    const auto b = std::make_shared<ViewNode>(Window("b"));
    const auto c = std::make_shared<ViewNode>(Window("c"));
    container->AddChild(a);
    container->AddChild(b);
    container->AddChild(c);
    a->SetFractions(.2, .2);
    b->SetFractions(.3, .3);
    assert(a->SwapWith(container->GetChildren()[1]));
    assert(container->GetChildren()[0] == b && container->GetChildren()[1] == a);
    assert(a->GetParent() == container && b->GetParent() == container);
    assert(a->GetWidthFraction() == .3 && b->GetWidthFraction() == .2);

    // Taking the old child by reference from its vector must survive removing a sibling.
    container->ReplaceChild(container->GetChildren()[2], container->GetChildren()[0]);
    assert(container->GetChildren().size() == 2);
    assert(container->GetChildren()[0] == a && container->GetChildren()[1] == b);
    assert(!c->GetParent());
}

void WorkspaceMoveAndDetach()
{
    TreeEngine engine;
    const auto a = engine.InsertWindow(Window("a"));
    const auto b = engine.InsertWindow(Window("b"));
    Arrange(engine);
    const auto old = engine.CaptureSnapshot();
    const auto workspace_id = old->active_workspace;
    assert(engine.MoveWindowToWorkspace(b->GetWindow(), "2"));
    const auto moved = engine.CaptureSnapshot();
    assert(moved->topology_revision > old->topology_revision);
    assert(Node(*moved, b->GetNodeId()).workspace != workspace_id);
    assert(Node(*moved, a->GetNodeId()).workspace == workspace_id);
    assert(engine.FindViewForWindow(b->GetWindow()) == b);

    assert(engine.SwitchWorkspace("2"));
    Arrange(engine);
    const auto active = engine.CaptureSnapshot();
    assert(active->focused_node == b->GetNodeId());
    assert(active->topology_revision == moved->topology_revision);
    assert(active->layout_revision > moved->layout_revision);
    assert(active->active_workspace == b->GetWorkspace()->GetNodeId());
    assert(engine.MoveWindowToWorkspace(b->GetWindow(), "2"));
    assert(engine.SwitchWorkspace("2"));
    Arrange(engine);
    assert(engine.CaptureSnapshot() == active);

    auto parent = b->GetParent();
    assert(parent->RemoveChild(b));
    const auto detached = engine.CaptureSnapshot();
    b->SetFractions(.1, .9);
    b->SetBounds({1, 2, 3, 4});
    assert(engine.CaptureSnapshot() == detached);
    parent->AddChild(b);
    assert(engine.CaptureSnapshot()->topology_revision > detached->topology_revision);
}

std::size_t LayoutNodeCount(const TreeSnapshot &snapshot)
{
    return snapshot.nodes.size() - snapshot.workspaces.size();
}

void WorkspaceCapacity()
{
    TreeEngine engine;
    const auto view = engine.InsertWindow(Window("a"));
    for (std::size_t i = 2; i <= contracts::kMaxLayoutWorkspaces; ++i) {
        assert(engine.GetOrCreateWorkspace(std::to_string(i)));
    }
    const auto full = engine.CaptureSnapshot();
    assert(full->workspaces.size() == contracts::kMaxLayoutWorkspaces);
    assert(!engine.GetOrCreateWorkspace("overflow"));
    assert(!engine.SwitchWorkspace("overflow"));
    assert(!engine.MoveWindowToWorkspace(view->GetWindow(), "overflow"));
    assert(engine.CaptureSnapshot() == full);
    assert(engine.GetFocusedNode() == view);
    assert(view->GetWindow()->IsFocused());

    assert(engine.SwitchWorkspace("2"));
    assert(engine.SwitchWorkspace("1"));
    assert(engine.GetFocusedNode() == view);
}

void NodeCapacity()
{
    TreeEngine engine;
    const auto root = engine.GetActiveWorkspace()->GetRootContainer();
    assert(engine.GetOrCreateWorkspace("2"));
    const auto anchor = engine.InsertWindow(Window("anchor"));
    const auto tabbed = engine.GroupTabbed(anchor, Window("tab"));
    assert(tabbed);
    const auto other_tab = tabbed->GetChildren()[1];
    assert(LayoutNodeCount(*engine.CaptureSnapshot()) == 5);

    std::shared_ptr<ViewNode> outer;
    for (std::size_t count = 5; count < contracts::kMaxLayoutNodes - 1; ++count) {
        outer = engine.InsertWindow(Window("filler"), Direction::Right, root);
        assert(outer);
    }
    const auto almost_full = engine.CaptureSnapshot();
    assert(LayoutNodeCount(*almost_full) == contracts::kMaxLayoutNodes - 1);
    assert(!engine.InsertWindow(Window("fission overflow"), Direction::Down, outer));
    assert(!engine.GroupTabbed(anchor, Window("group overflow")));
    assert(engine.CaptureSnapshot() == almost_full);

    // Appending to an existing group needs one node, while a new group needs two.
    assert(engine.GroupTabbed(tabbed, Window("last slot")) == tabbed);
    const auto last_tab = tabbed->GetChildren().back();
    engine.SetFocus(outer);
    const auto full = engine.CaptureSnapshot();
    assert(LayoutNodeCount(*full) == contracts::kMaxLayoutNodes);
    TreeEngine foreign;
    const auto foreign_view = foreign.InsertWindow(Window("foreign"));
    const auto foreign_state = foreign.CaptureSnapshot();
    assert(!engine.SwapNodes(outer, foreign_view));
    assert(!engine.SwapNodes(root, engine.GetOrCreateWorkspace("2")->GetRootContainer()));
    assert(!engine.SetLayoutMode(foreign_view, LayoutMode::Tabbed));
    assert(foreign.CaptureSnapshot() == foreign_state);
    assert(!engine.InsertWindow(Window("overflow")));
    assert(!engine.GroupTabbed(tabbed, Window("append overflow")));
    assert(!engine.SplitFocused(LayoutMode::SplitVertical));
    assert(!engine.GetOrCreateWorkspace("3"));
    assert(!engine.SwitchWorkspace("3"));
    assert(!engine.MoveWindowToWorkspace(outer->GetWindow(), "3"));
    assert(!outer->SetFractions(1000001, .5));
    assert(!outer->SetFractions(.5, 1000001));
    assert(engine.CaptureSnapshot() == full);
    assert(engine.GetFocusedNode() == outer && outer->GetWindow()->IsFocused());

    // Existing-workspace moves and mode changes add no nodes and remain available at the cap.
    assert(engine.SetLayoutMode(root, LayoutMode::SplitVertical));
    const auto vertical = engine.CaptureSnapshot();
    assert(LayoutNodeCount(*vertical) == contracts::kMaxLayoutNodes);
    assert(vertical->boundaries.size() <= contracts::kMaxLayoutBoundaries);
    assert(!engine.SetLayoutMode(root, LayoutMode::None));
    assert(engine.CaptureSnapshot() == vertical);
    assert(engine.MoveWindowToWorkspace(outer->GetWindow(), "2"));
    assert(engine.SwitchWorkspace("2"));
    assert(engine.GetFocusedNode() == outer);
    assert(engine.MoveWindowToWorkspace(outer->GetWindow(), "1"));
    assert(engine.SwitchWorkspace("1"));
    assert(LayoutNodeCount(*engine.CaptureSnapshot()) == contracts::kMaxLayoutNodes);

    // Removing nodes releases exact capacity, including wrappers removed by AutoPrune.
    assert(engine.RemoveWindow(last_tab->GetWindow()));
    engine.SetFocus(outer);
    assert(engine.SplitFocused(LayoutMode::SplitHorizontal));
    assert(LayoutNodeCount(*engine.CaptureSnapshot()) == contracts::kMaxLayoutNodes);
    assert(engine.RemoveWindow(anchor->GetWindow()));
    assert(LayoutNodeCount(*engine.CaptureSnapshot()) == contracts::kMaxLayoutNodes - 2);
    assert(other_tab->GetParent() == root);
    assert(engine.InsertWindow(Window("available fission"), Direction::Left, other_tab));
    assert(LayoutNodeCount(*engine.CaptureSnapshot()) == contracts::kMaxLayoutNodes);
}

} // namespace

int main()
{
    IdentityAndNoOp();
    AdjacencyLifetime();
    FractionsAndNestedPruning();
    GroupSplitAndCrossSwap();
    ActiveIdentityAcrossMutations();
    AliasedChildArguments();
    WorkspaceMoveAndDetach();
    WorkspaceCapacity();
    NodeCapacity();
}
