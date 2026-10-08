#include "prism/contracts/display_list_validation.hpp"
#include "prism/runtime/display_list_builder.hpp"
#include "prism/runtime/scene.hpp"

#include <cassert>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>
#include <variant>

using namespace prism;
using namespace prism::runtime;

namespace {
constexpr contracts::WindowId window{11};
constexpr contracts::ResourceId font{23};
constexpr contracts::NodeId target{3, 1};
constexpr contracts::Color panel_color{30, 40, 50, 220};
constexpr contracts::Color body_color{180, 10, 20, 255};
constexpr contracts::Color sibling_color{10, 20, 180, 255};

RenderNode Node(std::uint32_t index, contracts::LogicalRect bounds)
{
    RenderNode node;
    node.id = {index, 1};
    node.bounds = bounds;
    return node;
}

RenderTree Fixture()
{
    RenderTree tree;
    tree.root = {0, 1};
    tree.nodes.push_back(Node(0, {0, 0, 400, 300}));
    tree.nodes.push_back(Node(1, {0, 0, 400, 300}));
    tree.nodes.push_back(Node(2, {10, 20, 250, 200}));
    tree.nodes.push_back(Node(3, {30, 50, 160, 100}));
    tree.nodes.push_back(Node(4, {220, 30, 20, 30}));
    tree.nodes.push_back(Node(5, {40, 60, 100, 40}));
    tree.nodes.push_back(Node(6, {50, 110, 30, 20}));

    auto &root = tree.nodes[0];
    root.clip = true;
    root.clip_radius = 12;
    root.visuals.emplace_back(RectVisual{body_color});
    root.children = {{1, 1}, {2, 1}};
    tree.nodes[1].visuals.emplace_back(ImageVisual{{79}, contracts::ImageFit::Cover});

    auto &ancestor = tree.nodes[2];
    ancestor.presentation_scope = true;
    ancestor.presentation = {7, -3, 1.2, 0.8, 0.5, 0.5, 0.6};
    ancestor.clip = true;
    ancestor.contour = std::make_shared<const contracts::Contour>(
        contracts::Contour{{{10, 20}, {260, 20}, {260, 220}, {10, 220}}});
    ancestor.visuals.emplace_back(RectVisual{body_color});
    ancestor.children = {target, {4, 1}};

    auto &panel = tree.nodes[target.index];
    panel.clip = true;
    panel.clip_radius = 8;
    panel.visuals.emplace_back(RoundedRectVisual{8, panel_color});
    panel.visuals.emplace_back(ShadowVisual{8, 6, 3, {0, 0, 0, 90}, false});
    panel.visuals.emplace_back(ShadowVisual{8, 2, -1, {0, 0, 0, 40}, true});
    panel.visuals.emplace_back(BorderVisual{8, 1, {255, 255, 255, 90}});
    panel.children = {{5, 1}, {6, 1}};
    tree.nodes[4].visuals.emplace_back(RectVisual{sibling_color});

    auto &content = tree.nodes[5];
    content.presentation_scope = true;
    content.presentation.translate_y = 2;
    content.presentation.opacity = .8;
    content.visuals.emplace_back(
        TextVisual{{{{41, {1, 15}}, {42, {9, 15}}}, 17, 20}, 16, {240, 245, 250, 255}});
    content.visuals.emplace_back(IconVisual{contracts::VectorIcon::Save, {110, 160, 220, 255}, 4});
    tree.nodes[6].visible = false;
    tree.nodes[6].backdrop_blur = 20;
    tree.nodes[6].visuals.emplace_back(ImageVisual{{80}, contracts::ImageFit::Fill});
    return tree;
}

contracts::DisplayList Export(const RenderTree &tree, contracts::NodeId root = target)
{
    auto list = DisplayListBuilder::BuildSubtree(tree, root, window, font, 19);
    assert(list);
    contracts::ValidateDisplayList(*list);
    return std::move(*list);
}

template <typename T> const T &Command(const contracts::DisplayList &list, std::size_t index)
{
    assert(index < list.commands.size());
    return std::get<T>(list.commands[index]);
}

void CheckAncestorContextsAndSubtreeContents()
{
    const auto tree = Fixture();
    const auto list = Export(tree);
    assert(list.window == window && list.generation == 19);
    assert(list.commands.size() == 20);
    assert(Command<contracts::PushClipRoundedRect>(list, 0).radius == 12);
    const auto &matrix = Command<contracts::PushTransform>(list, 1).values;
    const double origin_x = 10 + 250 * .5;
    const double origin_y = 20 + 200 * .5;
    assert(matrix[0] == 1.2 && matrix[2] == 7 + origin_x * (1 - 1.2));
    assert(matrix[4] == .8 && matrix[5] == -3 + origin_y * (1 - .8));
    assert(Command<contracts::PushOpacity>(list, 2).opacity == .6);
    assert(Command<contracts::PushClipContour>(list, 3).contour == *tree.nodes[2].contour);

    const auto &outer = Command<contracts::RoundedRectShadow>(list, 4);
    assert(!outer.inset && outer.blur == 6 && outer.offset_y == 3);
    assert(outer.bounds == tree.nodes[3].bounds);
    assert(Command<contracts::PushClipRoundedRect>(list, 5).radius == 8);
    assert(Command<contracts::FillRoundedRect>(list, 6).color == panel_color);
    assert(Command<contracts::PushTransform>(list, 7).values[5] == 2);
    assert(Command<contracts::PushOpacity>(list, 8).opacity == .8);
    const auto &text = Command<contracts::DrawGlyphRun>(list, 9);
    assert(text.font == font && text.font_size == 16);
    const std::vector<contracts::GlyphPlacement> glyphs{{41, {41, 75}}, {42, {49, 75}}};
    assert(text.glyphs == glyphs);
    const auto &icon = Command<contracts::DrawIcon>(list, 10);
    assert(icon.icon == contracts::VectorIcon::Save);
    const contracts::LogicalRect icon_bounds{74, 64, 32, 32};
    assert(icon.bounds == icon_bounds);
    Command<contracts::PopOpacity>(list, 11);
    Command<contracts::PopTransform>(list, 12);
    assert(Command<contracts::RoundedRectShadow>(list, 13).inset);
    assert(Command<contracts::StrokeRoundedRect>(list, 14).width == 1);
    Command<contracts::PopClip>(list, 15);
    Command<contracts::PopClip>(list, 16);
    Command<contracts::PopOpacity>(list, 17);
    Command<contracts::PopTransform>(list, 18);
    Command<contracts::PopClip>(list, 19);
}

void CheckDetachedValueOwnershipAndContour()
{
    contracts::DisplayList exported;
    std::vector<contracts::DrawCommand> original;
    {
        auto tree = Fixture();
        tree.nodes[3].contour = std::make_shared<const contracts::Contour>(
            contracts::Contour{{{30, 50}, {190, 50}, {190, 150}, {30, 150}}});
        exported = Export(tree);
        original = exported.commands;
        assert(std::holds_alternative<contracts::ContourShadow>(exported.commands[4]));
        assert(std::holds_alternative<contracts::PushClipContour>(exported.commands[5]));
        assert(std::holds_alternative<contracts::FillContour>(exported.commands[6]));
        assert(std::holds_alternative<contracts::ContourShadow>(exported.commands[13]));
        assert(std::holds_alternative<contracts::StrokeContour>(exported.commands[14]));

        tree.nodes[2].contour.reset();
        tree.nodes[3].contour.reset();
        tree.nodes[3].visuals.clear();
        std::get<TextVisual>(tree.nodes[5].visuals[0]).shaped.glyphs.clear();
        tree.nodes.clear();
    }
    assert(exported.commands == original);
    contracts::ValidateDisplayList(exported);
}

void CheckResourceAndVisibilityBoundaries()
{
    auto tree = Fixture();
    assert(DisplayListBuilder::BuildSubtree(tree, target, window, font, 1));
    tree.nodes[6].visible = true;
    assert(!DisplayListBuilder::BuildSubtree(tree, target, window, font, 1));
    tree.nodes[6].visible = false;
    tree.nodes[3].visuals.emplace_back(ImageVisual{{90}, contracts::ImageFit::Contain});
    assert(!DisplayListBuilder::BuildSubtree(tree, target, window, font, 1));
    tree.nodes[3].visuals.pop_back();

    tree.nodes[5].backdrop_blur = 8;
    assert(!DisplayListBuilder::BuildSubtree(tree, target, window, font, 1));
    tree.nodes[5].backdrop_blur = 0;
    tree.nodes[3].backdrop_blur = 8;
    assert(!DisplayListBuilder::BuildSubtree(tree, target, window, font, 1));
    tree.nodes[3].backdrop_blur = 0;
    tree.nodes[2].backdrop_blur = 8;
    assert(!DisplayListBuilder::BuildSubtree(tree, target, window, font, 1));
    tree.nodes[2].backdrop_blur = 0;
    tree.nodes[0].backdrop_blur = 8;
    assert(!DisplayListBuilder::BuildSubtree(tree, target, window, font, 1));
    tree.nodes[0].backdrop_blur = 0;
    assert(DisplayListBuilder::BuildSubtree(tree, target, window, font, 1));

    tree.nodes[3].visible = false;
    assert(!DisplayListBuilder::BuildSubtree(tree, target, window, font, 1));
    tree.nodes[3].visible = true;
    tree.nodes[2].visible = false;
    assert(!DisplayListBuilder::BuildSubtree(tree, target, window, font, 1));
    tree.nodes[2].visible = true;
    tree.nodes[3].bounds.width = 0;
    assert(!DisplayListBuilder::BuildSubtree(tree, target, window, font, 1));
}

void CheckInvalidAndUnreachableTrees()
{
    auto tree = Fixture();
    assert(!DisplayListBuilder::BuildSubtree(tree, {}, window, font, 1));
    assert(!DisplayListBuilder::BuildSubtree(tree, {3, 2}, window, font, 1));
    assert(!DisplayListBuilder::BuildSubtree(tree, {20, 1}, window, font, 1));
    assert(!DisplayListBuilder::BuildSubtree(tree, target, {}, font, 1));
    tree.nodes[2].children = {{4, 1}};
    assert(!DisplayListBuilder::BuildSubtree(tree, target, window, font, 1));
    tree.nodes[2].children = {target};
    tree.nodes[3].children.push_back({0, 1});
    assert(!DisplayListBuilder::BuildSubtree(tree, target, window, font, 1));
    tree.nodes[3].children.pop_back();
    tree.nodes[3].children.push_back({20, 1});
    assert(!DisplayListBuilder::BuildSubtree(tree, target, window, font, 1));
    tree.nodes[3].children.pop_back();
    tree.nodes[2].presentation.opacity = std::numeric_limits<double>::quiet_NaN();
    assert(!DisplayListBuilder::BuildSubtree(tree, target, window, font, 1));
}

ShapedText Shape(std::string_view text, double size)
{
    ShapedText shaped;
    for (const auto character : text) {
        shaped.glyphs.push_back({static_cast<std::uint32_t>(character), {shaped.width, size}});
        shaped.width += 8;
    }
    shaped.height = size + 4;
    return shaped;
}

Blueprint Layout()
{
    Blueprint root;
    root.kind = Kind::Column;
    root.properties = {{DslProperty::Background, body_color}};

    Blueprint body;
    body.kind = Kind::Text;
    body.properties = {{DslProperty::Text, std::string("body")}, {DslProperty::Height, 40.0}};
    Blueprint panel;
    panel.region = "task";
    panel.properties = {{DslProperty::Background, panel_color}, {DslProperty::Height, 120.0}};
    Blueprint text;
    text.kind = Kind::Text;
    text.properties = {{DslProperty::Text, std::string("task")}};
    text.bindings = {{"task_text", DslProperty::Text}};
    panel.children.push_back(std::move(text));
    root.children = {std::move(body), std::move(panel)};
    return root;
}

void CheckSceneResolvedBuildGateAndBodyIndependence()
{
    Scene scene{Layout(), Shape, font};
    const auto task = scene.RegionId("task");
    assert(task);
    assert(!scene.CaptureTaskPaint(task));
    assert(scene.SetViewport({240, 200}));
    assert(!scene.CaptureTaskPaint(task));
    assert(scene.Build(window));
    const auto saved = scene.CaptureTaskPaint(task);
    assert(saved && saved->generation == 1);
    assert(saved->commands.size() == 2);
    assert(std::get<contracts::FillRect>(saved->commands[0]).color == panel_color);
    const auto original = saved->commands;

    assert(scene.SetBinding("task_text", std::string("latest")));
    assert(!scene.CaptureTaskPaint(task));
    assert(scene.Build(window));
    const auto updated = scene.CaptureTaskPaint(task);
    assert(updated && updated->generation == 2);
    assert(updated->commands != original);
    assert(saved->commands == original);

    assert(scene.SetBackground(scene.RootId(), {20, 120, 30, 255}));
    assert(!scene.CaptureTaskPaint(task));
    assert(scene.Build(window));
    const auto body_changed = scene.CaptureTaskPaint(task);
    assert(body_changed && body_changed->commands == updated->commands);
    assert(scene.SetViewport({240, 220}));
    assert(!scene.CaptureTaskPaint(task));
    assert(!scene.CaptureTaskPaint({task.index, task.generation + 1}));
}

void CheckCompositeOnlyBackdropGate()
{
    Scene scene{Layout(), Shape, font};
    const auto task = scene.RegionId("task");
    scene.SetViewport({240, 200});
    assert(scene.Build(window));
    assert(scene.CaptureTaskPaint(task));
    assert(scene.SetProperty(task, DslProperty::BackdropBlur, 8.0));
    assert(!Has(scene.PendingDirty(), Dirty::Layout));
    assert(!Has(scene.PendingDirty(), Dirty::Paint));
    assert(!scene.CaptureTaskPaint(task));
    assert(scene.SetProperty(task, DslProperty::BackdropBlur, 0.0));
    assert(scene.CaptureTaskPaint(task));

    const auto *task_input = scene.InputGeometry()->Find(task);
    assert(task_input && task_input->children.size() == 1);
    const auto child = task_input->children.front();
    assert(scene.SetProperty(child, DslProperty::BackdropBlur, 8.0));
    assert(!Has(scene.PendingDirty(), Dirty::Paint));
    assert(!scene.CaptureTaskPaint(task));
    assert(scene.SetProperty(child, DslProperty::BackdropBlur, 0.0));
    assert(scene.CaptureTaskPaint(task));

    assert(scene.SetProperty(scene.RootId(), DslProperty::BackdropBlur, 8.0));
    assert(!Has(scene.PendingDirty(), Dirty::Paint));
    assert(!scene.CaptureTaskPaint(task));
}

void CheckRenderTreeBackdropTracksResolvedStyle()
{
    SceneSnapshot snapshot;
    snapshot.root = {0, 1};
    SnapshotNode root;
    root.id = snapshot.root;
    root.bounds = {0, 0, 100, 60};
    root.style.background = panel_color;
    root.revision = 1;
    snapshot.nodes.push_back(root);
    const auto tree = RenderTreeBuilder::Build(snapshot);
    assert(tree.nodes[0].backdrop_blur == 0);
    snapshot.nodes[0].style.backdrop_blur = 8;
    // Composite-only state must not accidentally reuse a resource-free node.
    const auto changed = RenderTreeBuilder::Build(snapshot, &tree);
    assert(changed.nodes[0].backdrop_blur == 8);
    assert(changed.nodes[0].render_generation == tree.nodes[0].render_generation + 1);
    assert(!DisplayListBuilder::BuildSubtree(changed, changed.root, window, font, 1));
}
} // namespace

int main()
{
    CheckAncestorContextsAndSubtreeContents();
    CheckDetachedValueOwnershipAndContour();
    CheckResourceAndVisibilityBoundaries();
    CheckInvalidAndUnreachableTrees();
    CheckSceneResolvedBuildGateAndBodyIndependence();
    CheckCompositeOnlyBackdropGate();
    CheckRenderTreeBackdropTracksResolvedStyle();
    std::cout << "scene_task_paint_test: 7 groups passed\n";
}
