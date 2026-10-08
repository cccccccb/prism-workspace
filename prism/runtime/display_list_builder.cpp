#include "prism/runtime/display_list_builder.hpp"
#include "prism/contracts/display_list_validation.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <variant>

namespace prism::runtime {
namespace {
struct SubtreeOpacity {
    contracts::NodeId root;
    double value;
};

void EmitShadow(const RenderNode &node, const ShadowVisual &shadow, contracts::DisplayList &list)
{
    if (node.contour) {
        list.commands.emplace_back(contracts::ContourShadow{
            *node.contour, shadow.blur, shadow.offset_y, shadow.color, shadow.inset});
    } else {
        list.commands.emplace_back(contracts::RoundedRectShadow{
            node.bounds, shadow.radius, shadow.blur, shadow.offset_y, shadow.color, shadow.inset});
    }
}

void EmitClip(const RenderNode &node, contracts::DisplayList &list)
{
    if (node.contour) {
        list.commands.emplace_back(contracts::PushClipContour{*node.contour});
    } else if (node.clip_radius > 0) {
        list.commands.emplace_back(contracts::PushClipRoundedRect{node.bounds, node.clip_radius});
    } else {
        list.commands.emplace_back(contracts::PushClipRect{node.bounds});
    }
}

void EmitFill(const RenderNode &node, const RectVisual &visual, contracts::DisplayList &list)
{
    if (node.contour) {
        list.commands.emplace_back(contracts::FillContour{*node.contour, visual.color});
    } else {
        list.commands.emplace_back(contracts::FillRect{node.bounds, visual.color});
    }
}

void EmitFill(const RenderNode &node, const RoundedRectVisual &visual, contracts::DisplayList &list)
{
    if (node.contour) {
        list.commands.emplace_back(contracts::FillContour{*node.contour, visual.color});
    } else {
        list.commands.emplace_back(
            contracts::FillRoundedRect{node.bounds, visual.radius, visual.color});
    }
}

void EmitBorder(const RenderNode &node, const BorderVisual &border, contracts::DisplayList &list)
{
    if (node.contour) {
        list.commands.emplace_back(
            contracts::StrokeContour{*node.contour, border.width, border.color});
    } else {
        list.commands.emplace_back(
            contracts::StrokeRoundedRect{node.bounds, border.radius, border.width, border.color});
    }
}

void PushPresentation(const RenderNode &node, contracts::DisplayList &list)
{
    if (!node.presentation_scope) {
        return;
    }
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

void PopContext(const RenderNode &node, contracts::DisplayList &list)
{
    if (node.clip) {
        list.commands.emplace_back(contracts::PopClip{});
    }
    if (node.presentation_scope) {
        list.commands.emplace_back(contracts::PopOpacity{});
        list.commands.emplace_back(contracts::PopTransform{});
    }
}

void Emit(const RenderTree &tree, contracts::NodeId id, contracts::ResourceId font,
          contracts::DisplayList &list, const SubtreeOpacity *opacity = nullptr)
{
    const auto &node = tree.Get(id);
    const bool selected = opacity && id == opacity->root;
    if (!node.visible || node.bounds.width <= 0 || node.bounds.height <= 0 ||
        (selected && opacity->value == 0)) {
        return;
    }

    if (selected && opacity->value < 1) {
        list.commands.emplace_back(contracts::PushOpacity{opacity->value});
    }
    PushPresentation(node, list);
    for (const auto &visual : node.visuals) {
        if (const auto *shadow = std::get_if<ShadowVisual>(&visual); shadow && !shadow->inset) {
            EmitShadow(node, *shadow, list);
        }
    }
    if (node.clip) {
        EmitClip(node, list);
    }
    for (const auto &visual : node.visuals) {
        if (auto *selection = std::get_if<TextSelectionVisual>(&visual)) {
            for (auto rect : selection->rectangles) {
                rect.x += node.bounds.x;
                rect.y += node.bounds.y;
                list.commands.emplace_back(contracts::FillRect{rect, selection->color});
            }
        } else if (auto *rect = std::get_if<RectVisual>(&visual)) {
            EmitFill(node, *rect, list);
        } else if (auto *rect = std::get_if<RoundedRectVisual>(&visual)) {
            EmitFill(node, *rect, list);
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
        Emit(tree, child, font, list, opacity);
    }
    // Frame accents belong above contents; children cannot cover the edge.
    for (const auto &visual : node.visuals) {
        if (const auto *shadow = std::get_if<ShadowVisual>(&visual); shadow && shadow->inset) {
            EmitShadow(node, *shadow, list);
        } else if (const auto *border = std::get_if<BorderVisual>(&visual)) {
            EmitBorder(node, *border, list);
        }
    }
    PopContext(node, list);
    if (selected && opacity->value < 1) {
        list.commands.emplace_back(contracts::PopOpacity{});
    }
}

const RenderNode *FindNode(const RenderTree &tree, contracts::NodeId id)
{
    if (!id || id.index >= tree.nodes.size() || tree.nodes[id.index].id != id) {
        return nullptr;
    }
    return &tree.nodes[id.index];
}

bool Drawable(const RenderNode &node)
{
    return node.visible && node.bounds.width > 0 && node.bounds.height > 0;
}

bool FindPath(const RenderTree &tree, contracts::NodeId id, contracts::NodeId target,
              std::vector<contracts::NodeId> &path, std::vector<bool> &visited)
{
    const auto *node = FindNode(tree, id);
    if (!node || !Drawable(*node) || visited[id.index] || path.size() >= 256) {
        return false;
    }
    visited[id.index] = true;
    path.push_back(id);
    if (id == target) {
        return true;
    }
    for (const auto child : node->children) {
        if (FindPath(tree, child, target, path, visited)) {
            return true;
        }
    }
    path.pop_back();
    return false;
}

bool Exportable(const RenderTree &tree, contracts::NodeId id, std::vector<bool> &visited,
                std::size_t depth)
{
    const auto *node = FindNode(tree, id);
    if (!node || visited[id.index] || depth >= 256) {
        return false;
    }
    if (!Drawable(*node)) {
        return true;
    }
    visited[id.index] = true;
    if (node->backdrop_blur > 0 ||
        std::any_of(node->visuals.begin(), node->visuals.end(), [](const Visual &visual) {
            return std::holds_alternative<ImageVisual>(visual);
        })) {
        return false;
    }
    for (const auto child : node->children) {
        if (!Exportable(tree, child, visited, depth + 1)) {
            return false;
        }
    }
    return true;
}

bool FindExportablePath(const RenderTree &tree, contracts::NodeId root,
                        std::vector<contracts::NodeId> &path)
{
    std::vector<bool> visited(tree.nodes.size(), false);
    if (!FindPath(tree, tree.root, root, path, visited)) {
        return false;
    }
    for (const auto ancestor : path) {
        if (tree.Get(ancestor).backdrop_blur > 0) {
            return false;
        }
    }

    std::fill(visited.begin(), visited.end(), false);
    return Exportable(tree, root, visited, 0);
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

std::optional<contracts::DisplayList> DisplayListBuilder::BuildSubtree(const RenderTree &tree,
                                                                       contracts::NodeId root,
                                                                       contracts::WindowId window,
                                                                       contracts::ResourceId font,
                                                                       std::uint64_t generation)
{
    if (!window) {
        return std::nullopt;
    }
    std::vector<contracts::NodeId> path;
    if (!FindExportablePath(tree, root, path)) {
        return std::nullopt;
    }

    contracts::DisplayList list;
    list.window = window;
    list.generation = generation;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        const auto &ancestor = tree.Get(path[i]);
        PushPresentation(ancestor, list);
        if (ancestor.clip) {
            EmitClip(ancestor, list);
        }
    }
    Emit(tree, root, font, list);
    for (std::size_t i = path.size() - 1; i > 0; --i) {
        PopContext(tree.Get(path[i - 1]), list);
    }

    try {
        contracts::ValidateDisplayList(list);
    } catch (const std::invalid_argument &) {
        return std::nullopt;
    }
    return list;
}

std::optional<contracts::DisplayList>
DisplayListBuilder::BuildWithSubtreeOpacity(const RenderTree &tree, contracts::NodeId root,
                                            double opacity, contracts::WindowId window,
                                            contracts::ResourceId font, std::uint64_t generation)
{
    if (!std::isfinite(opacity) || opacity < 0 || opacity > 1) {
        throw std::invalid_argument("Subtree opacity must be finite and in [0,1]");
    }
    if (!window) {
        return std::nullopt;
    }
    std::vector<contracts::NodeId> path;
    if (!FindExportablePath(tree, root, path)) {
        return std::nullopt;
    }

    contracts::DisplayList list;
    list.window = window;
    list.generation = generation;
    const SubtreeOpacity selected{root, opacity};
    Emit(tree, tree.root, font, list, &selected);

    try {
        contracts::ValidateDisplayList(list);
    } catch (const std::invalid_argument &) {
        return std::nullopt;
    }
    return list;
}
} // namespace prism::runtime
