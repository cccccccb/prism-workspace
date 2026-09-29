#include "prism/runtime/render_tree.hpp"
#include <algorithm>
#include <stdexcept>

namespace prism::runtime {
namespace {
bool SameBounds(contracts::LogicalRect a, contracts::LogicalRect b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

void CollectVisibility(const SceneSnapshot &snapshot, std::vector<bool> &visible,
                       contracts::NodeId id, bool parent_visible)
{
    const auto &node = snapshot.Get(id);
    visible[id.index] = parent_visible && node.style.visible;
    for (auto child : node.children) {
        CollectVisibility(snapshot, visible, child, visible[id.index]);
    }
}
} // namespace

const RenderNode &RenderTree::Get(contracts::NodeId id) const
{
    if (!id || id.index >= nodes.size() || nodes[id.index].id != id) {
        throw std::out_of_range("Invalid render node");
    }
    return nodes[id.index];
}

RenderTree RenderTreeBuilder::Build(const SceneSnapshot &snapshot, const RenderTree *previous)
{
    RenderTree tree;
    tree.root = snapshot.root;
    tree.nodes.reserve(snapshot.nodes.size());
    std::vector<bool> visible(snapshot.nodes.size(), false);

    CollectVisibility(snapshot, visible, snapshot.root, true);

    for (const auto &source : snapshot.nodes) {
        const RenderNode *old = nullptr;
        if (previous && source.id.index < previous->nodes.size() &&
            previous->nodes[source.id.index].id == source.id) {
            old = &previous->nodes[source.id.index];
        }
        if (old && old->source_revision == source.revision &&
            SameBounds(old->bounds, source.bounds) && old->children == source.children &&
            old->visible == visible[source.id.index] && old->presentation == source.presentation) {
            tree.nodes.push_back(*old);
            continue;
        }
        RenderNode node;
        node.id = source.id;
        node.bounds = source.bounds;
        node.visible = visible[source.id.index];
        node.clip = source.style.clip;
        node.clip = node.clip || source.style.overflow == "clip";
        node.clip_radius = source.style.radius;
        node.presentation_scope = source.kind == Kind::Visual;
        node.presentation = source.presentation;
        node.children = source.children;
        node.source_revision = source.revision;
        node.render_generation = old ? old->render_generation + 1 : 1;

        if (!node.visible) {
            tree.nodes.push_back(std::move(node));
            continue;
        }
        if (source.style.shadow_color.a && source.style.shadow_blur > 0) {
            node.visuals.emplace_back(ShadowVisual{source.style.radius, source.style.shadow_blur,
                                                   source.style.shadow_y, source.style.shadow_color,
                                                   false});
        }
        if (source.style.background.a || source.kind == Kind::Visual) {
            if (source.style.radius > 0) {
                node.visuals.emplace_back(
                    RoundedRectVisual{source.style.radius, source.style.background});
            } else {
                node.visuals.emplace_back(RectVisual{source.style.background});
            }
        }
        if (source.kind != Kind::InteractionTarget && source.interaction.hovered &&
            snapshot.controls.hover.a) {
            node.visuals.emplace_back(
                RoundedRectVisual{source.style.radius, snapshot.controls.hover});
        }
        if (source.style.inner_shadow_color.a && source.style.inner_shadow_blur > 0) {
            node.visuals.emplace_back(
                ShadowVisual{source.style.radius, source.style.inner_shadow_blur,
                             source.style.inner_shadow_y, source.style.inner_shadow_color, true});
        }
        if (source.style.border_width > 0 && source.style.border_color.a) {
            node.visuals.emplace_back(BorderVisual{source.style.radius, source.style.border_width,
                                                   source.style.border_color});
        }
        if (source.kind != Kind::InteractionTarget && source.interaction.focusVisible &&
            snapshot.controls.focus_width > 0 && snapshot.controls.focus.a) {
            node.visuals.emplace_back(BorderVisual{
                source.style.radius, snapshot.controls.focus_width, snapshot.controls.focus});
        }

        if (source.kind == Kind::Text && !source.shaped.glyphs.empty()) {
            node.visuals.emplace_back(
                TextVisual{source.shaped, source.style.font_size, source.style.foreground});
        }
        if (source.kind == Kind::Image && source.image_ready) {
            node.visuals.emplace_back(ImageVisual{source.image, source.style.image_fit});
        }
        if ((source.kind == Kind::Icon || source.kind == Kind::IconButton) &&
            !source.icon.empty()) {
            constexpr std::string_view names[] = {
                "grid",     "music",  "settings",  "folder",  "terminal", "play",   "pause",
                "previous", "next",   "volume",    "wifi",    "battery",  "search", "sun",
                "moon",     "power",  "check",     "chevron", "refresh",  "cpu",    "memory",
                "heart",    "layers", "rectangle", "drop",    "wifi-off", "error"};
            const auto it = std::find(std::begin(names), std::end(names), source.icon);
            if (it != std::end(names)) {
                node.visuals.emplace_back(
                    IconVisual{static_cast<contracts::VectorIcon>(it - std::begin(names)),
                               source.style.foreground, source.style.padding});
            }
        }
        if (source.kind == Kind::Progress) {
            node.visuals.emplace_back(
                ProgressVisual{source.value, source.style.radius, source.style.foreground});
        }
        if (source.kind == Kind::Toggle) {
            node.visuals.emplace_back(
                ToggleVisual{source.checked, source.style.foreground, snapshot.controls});
        }
        tree.nodes.push_back(std::move(node));
    }
    return tree;
}
} // namespace prism::runtime
