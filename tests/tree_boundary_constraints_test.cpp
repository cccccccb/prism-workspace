#include "prism/tree/tree_engine.hpp"
#include "prism/wm/window.hpp"
#include <cassert>
#include <cmath>
#include <limits>

using namespace prism;
using namespace prism::tree;

namespace {

std::shared_ptr<wm::Window> Window(const char *name, float width, float height)
{
    auto window = std::make_shared<wm::Window>(name, name, core::Rect{}, nullptr);
    assert(window->SetMinimumSize(width, height));
    return window;
}

std::shared_ptr<ViewNode> View(const char *name, float width, float height)
{
    return std::make_shared<ViewNode>(Window(name, width, height));
}

void Near(double actual, double expected)
{
    assert(std::abs(actual - expected) < .0001);
}

void HorizontalPairAndCancellation()
{
    TreeEngine tree;
    const TreeLayoutConfig config{11, 0, false, 28};
    const core::Rect output{37, 49, 1201, 601};
    auto a = tree.InsertWindow(Window("a", 140, 60));
    auto b = tree.InsertWindow(Window("b", 160, 70));
    auto c = tree.InsertWindow(Window("c", 170, 80));
    assert(a->SetFractions(.25, .2));
    assert(b->SetFractions(.35, .3));
    assert(c->SetFractions(.4, .5));
    tree.Arrange(output, config);

    const auto original = tree.CaptureSnapshot();
    const auto boundary = original->boundaries.front().id;
    const auto saved = tree.CaptureBoundaryFractions(boundary);
    const auto bounds_a = a->GetBounds();
    const auto bounds_b = b->GetBounds();
    const auto bounds_c = c->GetBounds();
    assert(saved);
    const auto range = tree.GetBoundaryRange(boundary, config);
    assert(range && range->feasible && range->axis == LayoutMode::SplitHorizontal);
    Near(range->minimum, 145.5);
    Near(range->pair_fraction, .6);
    Near(range->position, bounds_a.width + 5.5);
    assert(range->parent == a->GetParent()->GetNodeId());
    assert(range->workspace == tree.GetActiveWorkspace()->GetNodeId());

    for (int position = -200; position <= 1500; position += 7) {
        assert(tree.ApplyBoundary(boundary, position, config));
        tree.Arrange(output, config);
        assert(a->GetBounds().width >= 140 && b->GetBounds().width >= 160);
        Near(a->GetBounds().width + b->GetBounds().width, bounds_a.width + bounds_b.width);
        Near(a->GetWidthFraction() + b->GetWidthFraction(), .6);
        Near(c->GetWidthFraction(), .4);
        Near(a->GetHeightFraction(), .2);
        Near(b->GetHeightFraction(), .3);
        assert(c->GetBounds() == bounds_c);
        assert(tree.CaptureSnapshot()->topology_revision == original->topology_revision);
    }

    const auto preview = tree.CaptureBoundaryFractions(boundary);
    assert(preview && tree.RestoreBoundaryFractions(*saved, *preview));
    tree.Arrange(output, config);
    assert(a->GetBounds() == bounds_a && b->GetBounds() == bounds_b && c->GetBounds() == bounds_c);
    assert(tree.CaptureSnapshot()->layout_revision > original->layout_revision);
    Near(original->nodes.back().width_fraction, .4);
}

void VerticalMiddlePair()
{
    TreeEngine tree;
    const TreeLayoutConfig config{9, 0, false, 28};
    const core::Rect output{-21, -31, 900, 1403};
    tree.GetActiveWorkspace()->GetRootContainer()->SetLayoutMode(LayoutMode::SplitVertical);
    auto a = tree.InsertWindow(Window("a", 50, 110));
    auto b = tree.InsertWindow(Window("b", 60, 120), Direction::Down);
    auto c = tree.InsertWindow(Window("c", 70, 130), Direction::Down);
    auto d = tree.InsertWindow(Window("d", 80, 140), Direction::Down);
    tree.Arrange(output, config);
    const auto root = tree.GetActiveWorkspace()->GetRootContainer();
    assert(root->GetLayoutMode() == LayoutMode::SplitVertical);
    const auto id = root->GetBoundaries()[1].id;
    const auto range = tree.GetBoundaryRange(id, config);
    assert(range && range->feasible);
    const auto first_bounds = a->GetBounds();
    const auto last_bounds = d->GetBounds();
    const auto before_fraction = b->GetHeightFraction();
    const auto after_fraction = c->GetHeightFraction();
    for (int position = -100; position <= 1700; position += 11) {
        assert(tree.ApplyBoundary(id, position, config));
        tree.Arrange(output, config);
        assert(a->GetBounds() == first_bounds && d->GetBounds() == last_bounds);
        assert(b->GetBounds().height >= 120 && c->GetBounds().height >= 130);
        Near(b->GetHeightFraction() + c->GetHeightFraction(), before_fraction + after_fraction);
    }
}

void RecursiveMinimumsAndHeaders()
{
    TreeEngine tree;
    const TreeLayoutConfig config{12, 0, false, 28};
    const auto root = tree.GetActiveWorkspace()->GetRootContainer();
    const auto tabs = std::make_shared<ContainerNode>(LayoutMode::Tabbed);
    const auto nested = std::make_shared<ContainerNode>(LayoutMode::SplitHorizontal);
    nested->AddChild(View("first", 120, 70));
    nested->AddChild(View("second", 80, 90));
    tabs->AddChild(nested);
    tabs->AddChild(View("tab", 140, 100));
    root->AddChild(tabs);
    root->AddChild(View("side", 180, 110));
    tree.Arrange({0, 0, 1000, 800}, config);

    const auto minimum = tree.GetMinimumSize(root, config);
    Near(minimum.width, 392);
    Near(minimum.height, 128);
    const auto tabs_min = tree.GetMinimumSize(tabs, config);
    Near(tabs_min.width, 200);
    Near(tabs_min.height, 128);
    const auto nested_min = tree.GetMinimumSize(nested, config);
    Near(nested_min.width, 200); // Tabbed content recursively receives zero inner gap.
    Near(nested_min.height, 90);
    const auto range = tree.GetBoundaryRange(nested->GetBoundaries().front().id, config);
    assert(range && range->feasible);
    Near(range->minimum, 120);
    assert(tree.ApplyBoundary(range->boundary, -100, config));
    tree.Arrange({0, 0, 1000, 800}, config);
    Near(nested->GetChildren().front()->GetBounds().width, 120);

    tabs->SetLayoutMode(LayoutMode::Stacked);
    const auto stacked_min = tree.GetMinimumSize(tabs, config);
    Near(stacked_min.width, 200);
    Near(stacked_min.height, 156);
    const TreeLayoutConfig short_header{12, 0, false, 2};
    Near(tree.GetMinimumSize(tabs, short_header).height, 132);

    const auto detached = std::make_shared<ContainerNode>(LayoutMode::SplitVertical);
    detached->AddChild(View("top", 100.25f, 70.25f));
    detached->AddChild(View("bottom", 80, 90));
    Near(tree.GetMinimumSize(detached, config).width, 101);
    Near(tree.GetMinimumSize(detached, config).height, 173);
}

void InfeasibleAndInvalidInputs()
{
    TreeEngine tree;
    const TreeLayoutConfig config{12, 0, false, 28};
    auto a = tree.InsertWindow(Window("a", 320, 100));
    auto b = tree.InsertWindow(Window("b", 320, 100));
    tree.Arrange({0, 0, 600, 400}, config);
    const auto id = tree.CaptureSnapshot()->boundaries.front().id;
    const auto snapshot = tree.CaptureSnapshot();
    assert(!tree.GetBoundaryRange(id, config)->feasible);
    assert(!tree.ApplyBoundary(id, 300, config));
    assert(!tree.GetBoundaryRange(0, config));
    assert(!tree.CaptureBoundaryFractions(0));
    assert(!tree.ApplyBoundary(0, 300, config));
    assert(!tree.ApplyBoundary(id, std::numeric_limits<double>::quiet_NaN(), config));
    assert(!tree.ApplyBoundary(id, std::numeric_limits<double>::infinity(), config));
    assert(!tree.ApplyBoundary(id, -std::numeric_limits<double>::infinity(), config));
    assert(tree.CaptureSnapshot() == snapshot);

    assert(!a->GetWindow()->SetMinimumSize(-1, 10));
    assert(!a->GetWindow()->SetMinimumSize(std::numeric_limits<float>::infinity(), 10));
    assert(!a->GetWindow()->SetMinimumSize(10, std::numeric_limits<float>::quiet_NaN()));
    Near(a->GetWindow()->GetMinimumWidth(), 320);
    Near(a->GetWindow()->GetMinimumHeight(), 100);
    tree.Arrange({0, 0, 1000, 90}, config);
    assert(!tree.GetBoundaryRange(id, config)->feasible); // Cross-axis cannot fit.
    tree.Arrange({0, 0, 1000, 400}, config);
    assert(tree.GetBoundaryRange(id, config)->feasible);
    assert(b->GetWindow()->SetMinimumSize(900, 100));
    assert(!tree.GetBoundaryRange(id, config)->feasible); // A committed constraint changed.

    const TreeLayoutConfig huge_gap{1000, 0, false, 28};
    assert(a->GetWindow()->SetMinimumSize(0, 0));
    assert(b->GetWindow()->SetMinimumSize(0, 0));
    tree.Arrange({0, 0, 100, 100}, huge_gap);
    assert(!tree.GetBoundaryRange(id, huge_gap)->feasible);
    tree.Arrange({0, 0, .5f, .5f}, config);
    assert(!tree.GetBoundaryRange(id, config)->feasible);
    assert(a->GetBounds().width > 0 && b->GetBounds().width > 0);
    TreeLayoutConfig invalid = config;
    invalid.header_height = std::numeric_limits<float>::quiet_NaN();
    assert(!tree.GetBoundaryRange(id, invalid)->feasible);
}

void RestoreCannotOverwriteNewWork()
{
    TreeEngine tree;
    const TreeLayoutConfig config{12, 0, false, 28};
    const core::Rect output{0, 0, 1500, 800};
    auto a = tree.InsertWindow(Window("a", 100, 100));
    auto b = tree.InsertWindow(Window("b", 100, 100));
    auto c = tree.InsertWindow(Window("c", 100, 100));
    tree.Arrange(output, config);
    const auto id = tree.CaptureSnapshot()->boundaries.front().id;
    const auto saved = tree.CaptureBoundaryFractions(id);
    assert(saved && tree.ApplyBoundary(id, 200, config));
    tree.Arrange(output, config);
    const auto preview = tree.CaptureBoundaryFractions(id);
    assert(preview);

    const auto sum = a->GetWidthFraction() + b->GetWidthFraction();
    assert(a->SetFractions(sum / 2, a->GetHeightFraction()));
    assert(b->SetFractions(sum / 2, b->GetHeightFraction()));
    const auto changed = tree.CaptureSnapshot();
    assert(!tree.RestoreBoundaryFractions(*saved, *preview));
    assert(tree.CaptureSnapshot() == changed);

    tree.Arrange(output, config);
    const auto expected = tree.CaptureBoundaryFractions(id);
    assert(expected);
    tree.InsertWindow(Window("new", 100, 100), Direction::Right, c);
    assert(!tree.RestoreBoundaryFractions(*saved, *expected));
    assert(tree.RemoveWindow(b->GetWindow()));
    assert(!tree.GetBoundaryRange(id, config));
    assert(!tree.RestoreBoundaryFractions(*saved, *expected));
    tree.InsertWindow(Window("replacement", 100, 100), Direction::Right, a);
    assert(!tree.CaptureBoundaryFractions(id));
}

void LastPairAndFractionalOutput()
{
    TreeEngine tree;
    const TreeLayoutConfig config{7, 0, false, 28};
    const core::Rect output{12.25f, 31.5f, 1000.25f, 600};
    auto a = tree.InsertWindow(Window("a", 50, 50));
    auto b = tree.InsertWindow(Window("b", 100, 50));
    auto c = tree.InsertWindow(Window("c", 110, 50));
    tree.Arrange(output, config);
    const auto id = tree.CaptureSnapshot()->boundaries.back().id;
    const auto first = a->GetBounds();
    for (int position = 0; position < 1200; position += 13) {
        assert(tree.ApplyBoundary(id, position, config));
        tree.Arrange(output, config);
        assert(a->GetBounds() == first);
        assert(b->GetBounds().width >= 100 && c->GetBounds().width >= 110);
        Near(c->GetBounds().x + c->GetBounds().width, output.x + output.width);
    }
}

void FractionalHeaderAndLargeOutput()
{
    TreeEngine tree;
    const TreeLayoutConfig config{6, 0, false, 28.5f};
    const auto root = tree.GetActiveWorkspace()->GetRootContainer();
    root->SetLayoutMode(LayoutMode::SplitVertical);
    const auto tabs = std::make_shared<ContainerNode>(LayoutMode::Tabbed);
    tabs->AddChild(View("tab", 100, 100));
    root->AddChild(tabs);
    root->AddChild(View("other", 100, 100));
    tree.Arrange({0, 0, 500, 1000}, config);
    auto id = root->GetBoundaries().front().id;
    assert(tree.ApplyBoundary(id, -100, config));
    tree.Arrange({0, 0, 500, 1000}, config);
    Near(tabs->GetBounds().height, 129);
    assert(tabs->GetChildren().front()->GetBounds().height >= 100);

    TreeEngine wide;
    auto a = wide.InsertWindow(Window("a", 1, 1));
    auto b = wide.InsertWindow(Window("b", 1, 1));
    wide.Arrange({0, 0, 100000, 1000}, config);
    id = wide.CaptureSnapshot()->boundaries.front().id;
    assert(wide.GetBoundaryRange(id, config)->feasible);
    assert(wide.ApplyBoundary(id, -100, config));
    wide.Arrange({0, 0, 100000, 1000}, config);
    assert(a->GetBounds().width >= 1 && a->GetBounds().width < 20);
    assert(wide.ApplyBoundary(id, 200000, config));
    wide.Arrange({0, 0, 100000, 1000}, config);
    assert(b->GetBounds().width >= 1 && b->GetBounds().width < 20);
}

} // namespace

int main()
{
    HorizontalPairAndCancellation();
    VerticalMiddlePair();
    RecursiveMinimumsAndHeaders();
    InfeasibleAndInvalidInputs();
    RestoreCannotOverwriteNewWork();
    LastPairAndFractionalOutput();
    FractionalHeaderAndLargeOutput();
}
