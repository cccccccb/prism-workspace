#include "prism/contracts/display_list_validation.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/display_list_builder.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/runtime/task_paint.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace prism;
using namespace prism::runtime;

namespace {
constexpr int width = 256;
constexpr int height = 160;
constexpr contracts::WindowId window{1};
constexpr contracts::ResourceId font{1};
constexpr contracts::NodeId task{3, 1};
using Pixels = std::vector<std::uint32_t>;

RenderNode Node(std::uint32_t index, contracts::LogicalRect bounds)
{
    RenderNode node;
    node.id = {index, 1};
    node.bounds = bounds;
    return node;
}

RenderTree PanelTree()
{
    RenderTree tree;
    tree.root = {0, 1};
    tree.nodes.push_back(Node(0, {0, 0, width, height}));
    tree.nodes.push_back(Node(1, {12, 22, 60, 28}));
    tree.nodes.push_back(Node(2, {104, 18, 130, 112}));
    tree.nodes.push_back(Node(3, {125, 35, 72, 54}));
    tree.nodes.push_back(Node(4, {134, 44, 32, 26}));
    tree.nodes.push_back(Node(5, {144, 44, 32, 26}));
    tree.nodes.push_back(Node(6, {146, 44, 12, 12}));

    auto &root = tree.nodes[0];
    root.clip = true;
    root.clip_radius = 12;
    root.visuals.emplace_back(RectVisual{{35, 50, 65, 255}});
    root.children = {{1, 1}, {2, 1}, {6, 1}};
    tree.nodes[1].visuals.emplace_back(RoundedRectVisual{4, {180, 95, 65, 255}});

    auto &ancestor = tree.nodes[2];
    ancestor.presentation_scope = true;
    ancestor.presentation.translate_x = 3;
    ancestor.presentation.translate_y = 2;
    ancestor.presentation.opacity = .75;
    ancestor.clip = true;
    ancestor.clip_radius = 12;
    ancestor.children = {task};

    auto &panel = tree.nodes[3];
    panel.clip = true;
    panel.clip_radius = 9;
    panel.visuals.emplace_back(ShadowVisual{9, 6, 3, {0, 0, 0, 160}, false});
    panel.visuals.emplace_back(RoundedRectVisual{9, {80, 110, 165, 220}});
    panel.visuals.emplace_back(ShadowVisual{9, 2, 1, {0, 0, 0, 90}, true});
    panel.visuals.emplace_back(BorderVisual{9, 1, {185, 205, 230, 160}});
    panel.children = {{4, 1}, {5, 1}};
    tree.nodes[4].visuals.emplace_back(RectVisual{{220, 60, 65, 255}});
    tree.nodes[5].visuals.emplace_back(RectVisual{{55, 180, 100, 255}});
    tree.nodes[6].visuals.emplace_back(RectVisual{{240, 170, 35, 255}});
    return tree;
}

contracts::DisplayList Opening(const RenderTree &tree, double opacity)
{
    auto list = DisplayListBuilder::BuildWithSubtreeOpacity(tree, task, opacity, window, font, 1);
    assert(list);
    contracts::ValidateDisplayList(*list);
    return std::move(*list);
}

Pixels Render(render_skia::RasterRenderer &renderer, const contracts::DisplayList &list)
{
    contracts::ValidateDisplayList(list);
    Pixels result(width * height);
    assert(renderer.Render(list, result.data(), width, height, width * 4));
    return result;
}

std::uint32_t Pixel(const Pixels &pixels, int x, int y)
{
    return pixels[static_cast<std::size_t>(y * width + x)];
}

bool Covers(const contracts::DamageRegion &damage, int x, int y)
{
    if (damage.full) {
        return true;
    }
    for (const auto &rect : damage.rects) {
        if (x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height) {
            return true;
        }
    }
    return false;
}

void CheckRepair(render_skia::RasterRenderer &renderer, const contracts::DisplayList &previous,
                 const contracts::DisplayList &next, bool full, bool changed_shadow)
{
    const auto before = Render(renderer, previous);
    const auto expected = Render(renderer, next);
    const auto damage =
        renderer.CompareDamage(&previous, next, width, height, renderer.ResourceEpoch());
    assert(damage.full == full);
    auto repaired = before;
    assert(renderer.Render(next, repaired.data(), width, height, width * 4, damage));
    assert(repaired == expected);

    std::size_t changed = 0;
    std::size_t shadows = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (Pixel(before, x, y) == Pixel(expected, x, y)) {
                continue;
            }
            assert(Covers(damage, x, y));
            ++changed;
            const bool panel = x >= 128 && x < 200 && y >= 37 && y < 91;
            const bool surrounding = x >= 108 && x < 220 && y >= 22 && y < 116;
            if (!panel && surrounding) {
                ++shadows;
            }
        }
    }
    assert(changed > 0);
    if (changed_shadow) {
        assert(shadows > 0);
    }
}

void CheckOpeningEndpointsAndOrdering()
{
    const auto tree = PanelTree();
    const auto normal = DisplayListBuilder::Build(tree, window, font, 1);
    const auto opaque = Opening(tree, 1);
    assert(opaque.commands == normal.commands);
    assert(opaque.window == normal.window && opaque.generation == normal.generation);

    auto hidden_tree = tree;
    hidden_tree.nodes[task.index].visible = false;
    const auto hidden = DisplayListBuilder::Build(hidden_tree, window, font, 1);
    const auto zero = Opening(tree, 0);
    assert(zero.commands == hidden.commands);
    const auto half = Opening(tree, .5);
    assert(half.commands.size() == normal.commands.size() + 2);
    assert(DisplayListBuilder::Build(tree, window, font, 1).commands == normal.commands);

    render_skia::RasterRenderer renderer("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    assert(renderer.Ready());
    const auto before = Render(renderer, zero);
    const auto halfway = Render(renderer, half);
    const auto after = Render(renderer, opaque);
    // A later overlapping sibling remains above the selected task. Emitting
    // a second task contribution at the end would cover this orange pixel.
    assert(Pixel(before, 151, 50) == 0xFFF0AA23);
    assert(Pixel(halfway, 151, 50) == Pixel(before, 151, 50));
    assert(Pixel(after, 151, 50) == Pixel(before, 151, 50));
    assert(Pixel(before, 25, 30) == Pixel(halfway, 25, 30));
    assert(Pixel(halfway, 25, 30) == Pixel(after, 25, 30));
    assert(Pixel(before, 180, 78) != Pixel(halfway, 180, 78));
    assert(Pixel(halfway, 180, 78) != Pixel(after, 180, 78));
    assert(Pixel(after, 0, 0) == 0); // The root rounded clip stays in place.
    CheckRepair(renderer, Opening(tree, .25), half, false, true);
    CheckRepair(renderer, half, zero, true, true);
}

RenderTree OverlapTree()
{
    RenderTree tree;
    tree.root = {0, 1};
    tree.nodes.push_back(Node(0, {0, 0, width, height}));
    tree.nodes.push_back(Node(1, {10, 10, 70, 40}));
    tree.nodes.push_back(Node(2, {15, 12, 60, 26}));
    tree.nodes.push_back(Node(3, {20, 16, 30, 20}));
    tree.nodes.push_back(Node(4, {30, 16, 30, 20}));
    tree.nodes[0].children = {{1, 1}};
    auto &ancestor = tree.nodes[1];
    ancestor.presentation_scope = true;
    ancestor.presentation.translate_x = 5;
    ancestor.presentation.translate_y = 3;
    ancestor.presentation.opacity = .8;
    ancestor.clip = true;
    ancestor.children = {{2, 1}};
    tree.nodes[2].children = {task, {4, 1}};
    tree.nodes[3].visuals.emplace_back(RectVisual{{255, 0, 0, 255}});
    tree.nodes[4].visuals.emplace_back(RectVisual{{0, 255, 0, 255}});
    return tree;
}

void CheckGroupCompositingAndNestedClip()
{
    const auto tree = OverlapTree();
    auto actual = DisplayListBuilder::BuildWithSubtreeOpacity(tree, {2, 1}, .5, window, font, 1);
    assert(actual);
    // An independently authored replay and PMA channel checks distinguish
    // group opacity from multiplying the overlapping children individually.
    const contracts::DisplayList reference{
        window,
        1,
        {contracts::PushTransform{{1, 0, 5, 0, 1, 3}}, contracts::PushOpacity{.8},
         contracts::PushClipRect{{10, 10, 70, 40}}, contracts::PushOpacity{.5},
         contracts::FillRect{{20, 16, 30, 20}, {255, 0, 0, 255}},
         contracts::FillRect{{30, 16, 30, 20}, {0, 255, 0, 255}}, contracts::PopOpacity{},
         contracts::PopClip{}, contracts::PopOpacity{}, contracts::PopTransform{}}};
    render_skia::RasterRenderer renderer("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    assert(renderer.Ready());
    const auto pixels = Render(renderer, *actual);
    assert(pixels == Render(renderer, reference));
    const auto overlap = Pixel(pixels, 40, 24);
    assert(((overlap >> 16) & 255) == 0 && (overlap & 255) == 0);
    assert(std::abs(static_cast<int>(overlap >> 24) - 102) <= 1);
    assert(std::abs(static_cast<int>((overlap >> 8) & 255) - 102) <= 1);
    assert(Pixel(pixels, 5, 24) == 0);

    auto clipped_tree = tree;
    clipped_tree.nodes[1].bounds = {38, 10, 20, 40};
    auto clipped =
        DisplayListBuilder::BuildWithSubtreeOpacity(clipped_tree, {2, 1}, .5, window, font, 1);
    assert(clipped);
    const auto clipped_pixels = Render(renderer, *clipped);
    assert(Pixel(clipped_pixels, 40, 24) == 0);
    assert(Pixel(clipped_pixels, 46, 24) == overlap);
}

contracts::DisplayList Body(contracts::Color color)
{
    return {window,
            1,
            {contracts::FillRect{{0, 0, width, height}, {35, 50, 65, 255}},
             contracts::FillRoundedRect{{12, 22, 60, 28}, 4, color},
             contracts::FillRect{{140, 52, 28, 20}, color}}};
}

void CheckClosingCompositionAndBodyRefresh()
{
    const auto tree = PanelTree();
    auto exported = DisplayListBuilder::BuildSubtree(tree, task, window, font, 1);
    assert(exported);
    TaskPaintFragment paint;
    paint.commands = exported->commands;
    const auto saved_paint = paint.commands;
    const auto body = Body({185, 85, 65, 255});
    const auto saved_body = body.commands;
    const auto zero = ComposeTaskPaint(body, paint, 0);
    const auto opaque = ComposeTaskPaint(body, paint, 1);
    const auto default_opaque = ComposeTaskPaint(body, paint);
    assert(zero.commands == body.commands);
    assert(zero.window == body.window && zero.generation == body.generation);
    assert(opaque.commands == default_opaque.commands);
    assert(opaque.commands.size() == body.commands.size() + paint.commands.size());
    const auto half = ComposeTaskPaint(body, paint, .5);
    assert(half.commands.size() == opaque.commands.size() + 2);
    assert(body.commands == saved_body && paint.commands == saved_paint);

    render_skia::RasterRenderer renderer("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    assert(renderer.Ready());
    const auto refreshed_body = Body({55, 170, 90, 255});
    const auto refreshed = ComposeTaskPaint(refreshed_body, paint, .5);
    const auto before = Render(renderer, half);
    const auto after = Render(renderer, refreshed);
    assert(Pixel(before, 25, 30) != Pixel(after, 25, 30));
    // Fresh body content also reaches pixels under the translucent retained
    // panel, rather than freezing the former whole-window frame.
    assert(Pixel(before, 153, 65) != Pixel(after, 153, 65));
    assert(paint.commands == saved_paint);
    CheckRepair(renderer, half, refreshed, false, false);
    CheckRepair(renderer, ComposeTaskPaint(refreshed_body, paint, .75), refreshed, false, true);
    CheckRepair(renderer, refreshed, ComposeTaskPaint(refreshed_body, paint, 0), true, true);
    const auto detached = ComposeTaskPaint({}, paint, .5);
    auto detached_window = detached;
    detached_window.window = window;
    const auto detached_pixels = Render(renderer, detached_window);
    assert(Pixel(detached_pixels, 0, 0) == 0 && Pixel(detached_pixels, 251, 155) == 0);
}

ShapedText UnusedShape(std::string_view, double)
{
    return {};
}

Blueprint SceneLayout()
{
    Blueprint root;
    root.kind = Kind::Column;
    root.properties = {{DslProperty::Background, contracts::Color{35, 50, 65, 255}}};
    Blueprint body;
    body.properties = {{DslProperty::Height, 20.0},
                       {DslProperty::Background, contracts::Color{180, 95, 65, 255}}};
    Blueprint panel;
    panel.region = "task";
    panel.properties = {{DslProperty::Height, 40.0},
                        {DslProperty::Background, contracts::Color{80, 110, 165, 220}}};
    root.children = {std::move(body), std::move(panel)};
    return root;
}

void CheckSceneReadonlyCaptureAndFallback()
{
    Scene scene{SceneLayout(), UnusedShape, font};
    const auto root = scene.RegionId("task");
    assert(root && !scene.CaptureTaskOpacityFrame(root, .5));
    scene.SetViewport({240, 160});
    assert(!scene.CaptureTaskOpacityFrame(root, .5));
    const auto normal = scene.Build(window);
    assert(normal);
    const auto stats = scene.GetRenderStats();
    const auto generation = scene.Generation();
    const auto transaction = scene.TransactionRevision();
    const auto pixels_revision = scene.PixelsRevision();
    const auto input = scene.InputGeometry();
    const auto dirty = scene.PendingDirty();
    const auto full = scene.CaptureTaskOpacityFrame(root, 1);
    const auto half = scene.CaptureTaskOpacityFrame(root, .5);
    const auto zero = scene.CaptureTaskOpacityFrame(root, 0);
    assert(full && half && zero && full->commands == normal->commands);
    assert(scene.GetRenderStats() == stats && scene.Generation() == generation);
    assert(scene.TransactionRevision() == transaction && scene.PixelsRevision() == pixels_revision);
    assert(scene.InputGeometry() == input && scene.PendingDirty() == dirty);
    assert(scene.CaptureTaskPaint(root));

    assert(scene.SetProperty(root, DslProperty::BackdropBlur, 8.0));
    assert(!Has(scene.PendingDirty(), Dirty::Paint));
    assert(!scene.CaptureTaskOpacityFrame(root, .5));
    assert(scene.SetProperty(root, DslProperty::BackdropBlur, 0.0));
    assert(scene.CaptureTaskOpacityFrame(root, .5));
    assert(scene.SetProperty(scene.RootId(), DslProperty::BackdropBlur, 8.0));
    assert(!scene.CaptureTaskOpacityFrame(root, 0));
    assert(scene.SetProperty(scene.RootId(), DslProperty::BackdropBlur, 0.0));
    assert(scene.SetBackground(root, {20, 40, 80, 255}));
    assert(!scene.CaptureTaskOpacityFrame(root, .5));
}

void CheckUnsupportedTargetsAndInvalidOpacity()
{
    auto tree = PanelTree();
    assert(!DisplayListBuilder::BuildWithSubtreeOpacity(tree, {}, .5, window, font, 1));
    assert(!DisplayListBuilder::BuildWithSubtreeOpacity(tree, {3, 2}, .5, window, font, 1));
    assert(!DisplayListBuilder::BuildWithSubtreeOpacity(tree, task, .5, {}, font, 1));
    tree.nodes[3].visuals.emplace_back(ImageVisual{{71}, contracts::ImageFit::Cover});
    assert(!DisplayListBuilder::BuildWithSubtreeOpacity(tree, task, .5, window, font, 1));
    tree.nodes[3].visuals.pop_back();
    tree.nodes[2].backdrop_blur = 8;
    assert(!DisplayListBuilder::BuildWithSubtreeOpacity(tree, task, .5, window, font, 1));
    tree.nodes[2].backdrop_blur = 0;
    tree.nodes[5].backdrop_blur = 8;
    assert(!DisplayListBuilder::BuildWithSubtreeOpacity(tree, task, .5, window, font, 1));
    tree.nodes[5].backdrop_blur = 0;
    tree.nodes[2].children.clear();
    assert(!DisplayListBuilder::BuildWithSubtreeOpacity(tree, task, .5, window, font, 1));
    tree = PanelTree();

    const auto body = Body({185, 85, 65, 255});
    TaskPaintFragment paint;
    paint.commands = DisplayListBuilder::BuildSubtree(tree, task, window, font, 1)->commands;
    const auto saved_body = body.commands;
    const auto saved_paint = paint.commands;
    Scene scene{SceneLayout(), UnusedShape, font};
    for (const auto opacity :
         {-0.1, 1.1, std::numeric_limits<double>::quiet_NaN(),
          std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
        bool rejected = false;
        try {
            (void)DisplayListBuilder::BuildWithSubtreeOpacity(tree, task, opacity, window, font, 1);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        assert(rejected);
        rejected = false;
        try {
            (void)ComposeTaskPaint(body, paint, opacity);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        assert(rejected);
        rejected = false;
        try {
            (void)scene.CaptureTaskOpacityFrame(scene.RegionId("task"), opacity);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        assert(rejected);
    }
    assert(body.commands == saved_body && paint.commands == saved_paint);
    assert(ComposeTaskPaint(body, {}, .5).commands == body.commands);
}

void Run(std::string_view name, void (*check)())
{
    check();
    std::cout << "PASS " << name << '\n';
}
} // namespace

int main()
{
    Run("opening endpoint baselines, order and shadow repair", CheckOpeningEndpointsAndOrdering);
    Run("group opacity and unchanged nested clips", CheckGroupCompositingAndNestedClip);
    Run("closing composition, fresh body and damage replay", CheckClosingCompositionAndBodyRefresh);
    Run("readonly Scene capture and unsupported fallback", CheckSceneReadonlyCaptureAndFallback);
    Run("unsupported resources and atomic invalid opacity",
        CheckUnsupportedTargetsAndInvalidOpacity);
}
