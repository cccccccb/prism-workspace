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
}
