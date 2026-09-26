#include "prism/runtime/display_list_builder.hpp"
#include <variant>

namespace prism::runtime {
namespace {
void Emit(const RenderTree& tree, contracts::NodeId id, contracts::ResourceId font,
          contracts::DisplayList& list) {
    const auto& node = tree.Get(id);
    if (node.bounds.width <= 0 || node.bounds.height <= 0) return;
    if (node.clip) list.commands.emplace_back(contracts::PushClipRect{node.bounds});
    for (const auto& visual : node.visuals) {
        if (auto* rect = std::get_if<RectVisual>(&visual))
            list.commands.emplace_back(contracts::FillRect{node.bounds, rect->color});
        else if (auto* rect = std::get_if<RoundedRectVisual>(&visual))
            list.commands.emplace_back(contracts::FillRoundedRect{node.bounds, rect->radius, rect->color});
        else if (auto* image = std::get_if<ImageVisual>(&visual))
            list.commands.emplace_back(contracts::DrawImage{image->image, node.bounds});
        else if (auto* text = std::get_if<TextVisual>(&visual)) {
            contracts::DrawGlyphRun run;
            run.font = font;
            run.color = text->color;
            run.font_size = text->font_size;
            run.glyphs = text->shaped.glyphs;
            for (auto& glyph : run.glyphs) {
                glyph.origin.x += node.bounds.x;
                glyph.origin.y += node.bounds.y;
            }
            list.commands.emplace_back(std::move(run));
        }
    }
    for (auto child : node.children) Emit(tree, child, font, list);
    if (node.clip) list.commands.emplace_back(contracts::PopClip{});
}
} // namespace

contracts::DisplayList DisplayListBuilder::Build(const RenderTree& tree, contracts::WindowId window,
                                                  contracts::ResourceId font, std::uint64_t generation) {
    contracts::DisplayList list;
    list.window = window;
    list.generation = generation;
    Emit(tree, tree.root, font, list);
    return list;
}
} // namespace prism::runtime
