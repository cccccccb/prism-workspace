#include "prism/contracts/display_list_validation.hpp"
#include "prism/runtime/display_list_builder.hpp"
#include "prism/runtime/layout_engine.hpp"
#include "prism/runtime/render_tree.hpp"
#include <cassert>
#include <memory>
#include <variant>

namespace {
void CheckContourReplay()
{
    using namespace prism;
    const contracts::Contour contour{{{10, 20}, {70, 20}, {70, 40}, {40, 40}, {40, 80}, {10, 80}}};
    runtime::SceneSnapshot snapshot;
    snapshot.root = {0, 1};
    snapshot.controls.hover = {255, 255, 255, 80};
    snapshot.controls.focus = {20, 100, 240, 255};
    snapshot.controls.focus_width = 2;

    runtime::SnapshotNode root;
    root.id = snapshot.root;
    root.bounds = {10, 20, 60, 60};
    root.contour = std::make_shared<const contracts::Contour>(contour);
    root.style.background = {10, 20, 30, 255};
    root.style.radius = 900;
    root.style.clip = true;
    root.style.shadow_blur = 6;
    root.style.shadow_y = 3;
    root.style.shadow_color = {0, 0, 0, 80};
    root.style.inner_shadow_blur = 2;
    root.style.inner_shadow_y = -1;
    root.style.inner_shadow_color = {0, 0, 0, 40};
    root.style.border_width = 1;
    root.style.border_color = {255, 255, 255, 120};
    root.interaction.hovered = true;
    root.interaction.focusVisible = true;
    root.children = {{1, 1}};
    runtime::SnapshotNode child;
    child.id = {1, 1};
    child.bounds = {15, 25, 20, 20};
    child.style.background = {200, 100, 50, 255};
    snapshot.nodes = {root, child};

    const auto tree = runtime::RenderTreeBuilder::Build(snapshot);
    assert(tree.Get(root.id).contour == snapshot.Get(root.id).contour);
    assert(tree.Get(root.id).clip_radius == 0);
    const auto list = runtime::DisplayListBuilder::Build(tree, {1}, {}, 1);
    contracts::ValidateDisplayList(list);
    assert(list.commands.size() == 9);
    const auto &outer = std::get<contracts::ContourShadow>(list.commands[0]);
    assert(outer.contour == contour && !outer.inset && outer.offset_y == 3);
    assert(std::get<contracts::PushClipContour>(list.commands[1]).contour == contour);
    assert(std::get<contracts::FillContour>(list.commands[2]).contour == contour);
    const auto &hover = std::get<contracts::FillContour>(list.commands[3]);
    assert(hover.contour == contour && hover.color == snapshot.controls.hover);
    assert(std::holds_alternative<contracts::FillRect>(list.commands[4]));
    const auto &inner = std::get<contracts::ContourShadow>(list.commands[5]);
    assert(inner.contour == contour && inner.inset && inner.offset_y == -1);
    assert(std::get<contracts::StrokeContour>(list.commands[6]).contour == contour);
    const auto &focus = std::get<contracts::StrokeContour>(list.commands[7]);
    assert(focus.contour == contour && focus.width == snapshot.controls.focus_width);
    assert(std::holds_alternative<contracts::PopClip>(list.commands[8]));

    snapshot.Get(root.id).contour = std::make_shared<const contracts::Contour>(contour);
    const auto same = runtime::RenderTreeBuilder::Build(snapshot, &tree);
    assert(same.Get(root.id).render_generation == tree.Get(root.id).render_generation);
    assert(same.Get(root.id).contour == snapshot.Get(root.id).contour);

    const contracts::Contour changed{{{10, 20}, {70, 20}, {70, 50}, {30, 50}, {30, 80}, {10, 80}}};
    snapshot.Get(root.id).contour = std::make_shared<const contracts::Contour>(changed);
    const auto next = runtime::RenderTreeBuilder::Build(snapshot, &same);
    assert(next.Get(root.id).render_generation == same.Get(root.id).render_generation + 1);
    const auto changed_list = runtime::DisplayListBuilder::Build(next, {1}, {}, 2);
    contracts::ValidateDisplayList(changed_list);
    assert(std::get<contracts::FillContour>(changed_list.commands[2]).contour == changed);
    assert(std::get<contracts::FillContour>(list.commands[2]).contour == contour);

    snapshot.Get(root.id).style.clip = false;
    ++snapshot.Get(root.id).revision;
    const auto unclipped = runtime::RenderTreeBuilder::Build(snapshot, &next);
    const auto unclipped_list = runtime::DisplayListBuilder::Build(unclipped, {1}, {}, 3);
    assert(!unclipped.Get(root.id).clip && unclipped_list.commands.size() == 7);
    assert(std::holds_alternative<contracts::ContourShadow>(unclipped_list.commands[0]));

    snapshot.Get(root.id).contour.reset();
    const auto rounded = runtime::RenderTreeBuilder::Build(snapshot, &unclipped);
    assert(rounded.Get(root.id).render_generation == unclipped.Get(root.id).render_generation + 1);
    assert(rounded.Get(root.id).clip_radius == 30);
    const auto rounded_list = runtime::DisplayListBuilder::Build(rounded, {1}, {}, 4);
    assert(std::get<contracts::RoundedRectShadow>(rounded_list.commands[0]).radius == 30);
}
} // namespace

int main()
{
    CheckContourReplay();

    using namespace prism;
    runtime::SceneSnapshot snapshot;
    snapshot.root = {0, 1};
    runtime::SnapshotNode root;
    root.id = snapshot.root;
    root.kind = runtime::Kind::Box;
    root.style.background = {10, 20, 30, 255};
    root.style.clip = true;
    root.children.push_back({1, 1});
    runtime::SnapshotNode text;
    text.id = {1, 1};
    text.kind = runtime::Kind::Text;
    text.text = "A";
    snapshot.nodes = {root, text};
    runtime::LayoutEngine::Compute(snapshot, {100, 60}, [](std::string_view, double) {
        runtime::ShapedText result;
        result.glyphs.push_back({42, {0, 16}});
        result.width = 8;
        result.height = 20;
        return result;
    });
    assert(snapshot.Get({1, 1}).bounds.width == 100);
    auto first = runtime::RenderTreeBuilder::Build(snapshot);
    auto list = runtime::DisplayListBuilder::Build(first, contracts::WindowId{1},
                                                   contracts::ResourceId{7}, 1);
    assert(list.commands.size() == 4);
    assert(std::holds_alternative<contracts::PushClipRect>(list.commands[0]));
    assert(std::holds_alternative<contracts::FillRect>(list.commands[1]));
    auto *run = std::get_if<contracts::DrawGlyphRun>(&list.commands[2]);
    assert(run && run->font.value == 7 && run->glyphs[0].glyph_index == 42);
    assert(std::holds_alternative<contracts::PopClip>(list.commands[3]));
    auto same = runtime::RenderTreeBuilder::Build(snapshot, &first);
    assert(same.Get({0, 1}).render_generation == 1);
    assert(same.Get({1, 1}).render_generation == 1);
    snapshot.Get({0, 1}).style.background = {40, 50, 60, 255};
    ++snapshot.Get({0, 1}).revision;
    auto changed = runtime::RenderTreeBuilder::Build(snapshot, &same);
    assert(changed.Get({0, 1}).render_generation == 2);
    assert(changed.Get({1, 1}).render_generation == 1);

    runtime::SnapshotNode decoration;
    decoration.id = {2, 1};
    decoration.kind = runtime::Kind::Visual;
    decoration.bounds = {20, 10, 40, 20};
    decoration.style.background = {255, 0, 0, 255};
    decoration.presentation.scale_x = 1.5;
    decoration.presentation.translate_x = 3;
    decoration.presentation.opacity = 0.5;
    decoration.children = {{1, 1}};
    snapshot.nodes.push_back(decoration);
    snapshot.Get(snapshot.root).children = {decoration.id};
    ++snapshot.Get(snapshot.root).revision;
    const auto transformed = runtime::RenderTreeBuilder::Build(snapshot, &changed);
    const auto decorated = runtime::DisplayListBuilder::Build(transformed, {1}, {7}, 2);
    assert(decorated.commands.size() == 9);
    const auto &matrix = std::get<contracts::PushTransform>(decorated.commands[2]).values;
    assert(matrix[0] == 1.5 && matrix[2] == -17 && matrix[4] == 1 && matrix[5] == 0);
    assert(std::get<contracts::PushOpacity>(decorated.commands[3]).opacity == 0.5);
    assert(std::holds_alternative<contracts::DrawGlyphRun>(decorated.commands[5]));
    assert(std::holds_alternative<contracts::PopOpacity>(decorated.commands[6]));
    assert(std::holds_alternative<contracts::PopTransform>(decorated.commands[7]));

    snapshot.Get(decoration.id).presentation = {};
    const auto identity = runtime::RenderTreeBuilder::Build(snapshot, &transformed);
    const auto identity_list = runtime::DisplayListBuilder::Build(identity, {1}, {7}, 3);
    assert(identity_list.commands.size() == decorated.commands.size());
    assert(std::get<contracts::PushOpacity>(identity_list.commands[3]).opacity == 1);
    assert(identity.Get(decoration.id).render_generation ==
           transformed.Get(decoration.id).render_generation + 1);

    snapshot.Get(decoration.id).style.background.a = 0;
    ++snapshot.Get(decoration.id).revision;
    const auto clear_tree = runtime::RenderTreeBuilder::Build(snapshot, &identity);
    const auto clear_list = runtime::DisplayListBuilder::Build(clear_tree, {1}, {7}, 4);
    assert(clear_list.commands.size() == identity_list.commands.size());
    assert(std::get<contracts::FillRect>(clear_list.commands[4]).color.a == 0);

    auto &target = snapshot.Get(decoration.id);
    target.kind = runtime::Kind::InteractionTarget;
    target.style.background.a = 255;
    target.interaction.hovered = true;
    target.interaction.focusVisible = true;
    ++target.revision;
    snapshot.controls.hover = {255, 255, 255, 80};
    snapshot.controls.focus = {20, 100, 240, 255};
    snapshot.controls.focus_width = 2;
    const auto target_tree = runtime::RenderTreeBuilder::Build(snapshot, &identity);
    assert(!target_tree.Get(decoration.id).presentation_scope);
    assert(target_tree.Get(decoration.id).visuals.size() == 1);
}
