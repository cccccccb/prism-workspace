#include "prism/runtime/display_list_builder.hpp"
#include "prism/runtime/layout_engine.hpp"
#include "prism/runtime/render_tree.hpp"
#include <cassert>
#include <variant>

int main()
{
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
