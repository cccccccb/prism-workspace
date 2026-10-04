#include "prism/runtime/display_list_builder.hpp"
#include <algorithm>
#include <variant>

namespace prism::runtime {
namespace {
void Emit(const RenderTree &tree, contracts::NodeId id, contracts::ResourceId font,
          contracts::DisplayList &list)
{
    const auto &node = tree.Get(id);
    if (!node.visible || node.bounds.width <= 0 || node.bounds.height <= 0) {
        return;
    }
    if (node.presentation_scope) {
        const auto &value = node.presentation;
        const auto origin_x = node.bounds.x + node.bounds.width * value.origin_x;
        const auto origin_y = node.bounds.y + node.bounds.height * value.origin_y;
        list.commands.emplace_back(contracts::PushTransform{
            {value.scale_x, 0, value.translate_x + origin_x * (1 - value.scale_x), 0, value.scale_y,
             value.translate_y + origin_y * (1 - value.scale_y)}});
        // Keep scopes even at their identity values. Animation samples then
        // change values without repeatedly changing DisplayList structure.
        list.commands.emplace_back(contracts::PushOpacity{value.opacity});
    }
    for (const auto &visual : node.visuals) {
        if (const auto *shadow = std::get_if<ShadowVisual>(&visual); shadow && !shadow->inset) {
            list.commands.emplace_back(contracts::RoundedRectShadow{
                node.bounds, shadow->radius, shadow->blur, shadow->offset_y, shadow->color, false});
        }
    }
    if (node.clip) {
        if (node.clip_radius > 0) {
            list.commands.emplace_back(
                contracts::PushClipRoundedRect{node.bounds, node.clip_radius});
        } else {
            list.commands.emplace_back(contracts::PushClipRect{node.bounds});
        }
    }
    for (const auto &visual : node.visuals) {
        if (auto *selection = std::get_if<TextSelectionVisual>(&visual)) {
            for (auto rect : selection->rectangles) {
                rect.x += node.bounds.x;
                rect.y += node.bounds.y;
                list.commands.emplace_back(contracts::FillRect{rect, selection->color});
            }
        } else if (auto *rect = std::get_if<RectVisual>(&visual)) {
            list.commands.emplace_back(contracts::FillRect{node.bounds, rect->color});
        } else if (auto *rect = std::get_if<RoundedRectVisual>(&visual)) {
            list.commands.emplace_back(
                contracts::FillRoundedRect{node.bounds, rect->radius, rect->color});
        } else if (auto *image = std::get_if<ImageVisual>(&visual)) {
            list.commands.emplace_back(contracts::DrawImage{image->image, node.bounds, image->fit});
        } else if (auto *icon = std::get_if<IconVisual>(&visual)) {
            const auto size =
                std::max(0.0, std::min(node.bounds.width, node.bounds.height) - 2 * icon->padding);
            list.commands.emplace_back(
                contracts::DrawIcon{icon->icon,
                                    {node.bounds.x + (node.bounds.width - size) / 2,
                                     node.bounds.y + (node.bounds.height - size) / 2, size, size},
                                    icon->color});
        } else if (auto *progress = std::get_if<ProgressVisual>(&visual)) {
            auto bounds = node.bounds;
            bounds.width *= progress->value;
            list.commands.emplace_back(
                contracts::FillRoundedRect{bounds, progress->radius, progress->color});
        } else if (auto *toggle = std::get_if<ToggleVisual>(&visual)) {
            const auto radius =
                std::min(toggle->controls.toggle_track_radius, node.bounds.height / 2);
            if (toggle->checked) {
                list.commands.emplace_back(
                    contracts::FillRoundedRect{node.bounds, radius, toggle->color});
            }
            const auto inset = std::min(toggle->controls.toggle_inset,
                                        std::min(node.bounds.width, node.bounds.height) / 2);
            const auto diameter =
                std::max(0.0, std::min(node.bounds.width, node.bounds.height) - 2 * inset);
            list.commands.emplace_back(contracts::FillRoundedRect{
                {node.bounds.x + (toggle->checked ? node.bounds.width - diameter - inset : inset),
                 node.bounds.y + inset, diameter, diameter},
                std::min(toggle->controls.toggle_knob_radius, diameter / 2),
                toggle->controls.toggle_knob});
        } else if (auto *text = std::get_if<TextVisual>(&visual)) {
            contracts::DrawGlyphRun run;
            run.font = font;
            run.color = text->color;
            run.font_size = text->font_size;
            run.glyphs = text->shaped.glyphs;
            for (auto &glyph : run.glyphs) {
                glyph.origin.x += node.bounds.x;
                glyph.origin.y += node.bounds.y;
            }
            list.commands.emplace_back(std::move(run));
        }
    }
    for (auto child : node.children) {
        Emit(tree, child, font, list);
    }
    // Frame accents belong above contents; children cannot cover the edge.
    for (const auto &visual : node.visuals) {
        if (const auto *shadow = std::get_if<ShadowVisual>(&visual); shadow && shadow->inset) {
            list.commands.emplace_back(contracts::RoundedRectShadow{
                node.bounds, shadow->radius, shadow->blur, shadow->offset_y, shadow->color, true});
        } else if (const auto *border = std::get_if<BorderVisual>(&visual)) {
            list.commands.emplace_back(contracts::StrokeRoundedRect{node.bounds, border->radius,
                                                                    border->width, border->color});
        }
    }
    if (node.clip) {
        list.commands.emplace_back(contracts::PopClip{});
    }
    if (node.presentation_scope) {
        list.commands.emplace_back(contracts::PopOpacity{});
        list.commands.emplace_back(contracts::PopTransform{});
    }
}
} // namespace

contracts::DisplayList DisplayListBuilder::Build(const RenderTree &tree, contracts::WindowId window,
                                                 contracts::ResourceId font,
                                                 std::uint64_t generation)
{
    contracts::DisplayList list;
    list.window = window;
    list.generation = generation;
    Emit(tree, tree.root, font, list);
    return list;
}
} // namespace prism::runtime
