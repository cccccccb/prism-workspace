#include "prism/runtime/render_tree.hpp"
#include "prism/contracts/rounded_region.hpp"
#include <algorithm>
#include <stdexcept>

namespace prism::runtime {
namespace {
bool SameBounds(contracts::LogicalRect a, contracts::LogicalRect b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

bool SameContour(const std::shared_ptr<const contracts::Contour> &a,
                 const std::shared_ptr<const contracts::Contour> &b)
{
    return a == b || (a && b && *a == *b);
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
            old->visible == visible[source.id.index] && old->presentation == source.presentation &&
            old->backdrop_blur == source.style.backdrop_blur &&
            SameContour(old->contour, source.contour)) {
            tree.nodes.push_back(*old);
            tree.nodes.back().contour = source.contour;
            continue;
        }
        const auto radius =
            source.contour ? 0
                           : contracts::NormalizeRoundedRegion({source.bounds, source.style.radius})
                                 .corner_radius;
        RenderNode node;
        node.id = source.id;
        node.bounds = source.bounds;
        node.contour = source.contour;
        node.visible = visible[source.id.index];
        node.clip = source.style.clip || IsFloatingKind(source.kind) ||
                    source.kind == Kind::ScrollView || source.kind == Kind::TextField ||
                    source.kind == Kind::TextArea;
        node.clip = node.clip || source.style.overflow == "clip";
        node.clip_radius = radius;
        node.presentation_scope = source.kind == Kind::Visual;
        node.backdrop_blur = source.style.backdrop_blur;
        node.presentation = source.presentation;
        node.children = source.children;
        node.source_revision = source.revision;
        node.render_generation = old ? old->render_generation + 1 : 1;

        if (!node.visible) {
            tree.nodes.push_back(std::move(node));
            continue;
        }
        if (source.style.shadow_color.a && source.style.shadow_blur > 0) {
            node.visuals.emplace_back(ShadowVisual{radius, source.style.shadow_blur,
                                                   source.style.shadow_y, source.style.shadow_color,
                                                   false});
        }
        if (source.style.background.a || source.kind == Kind::Visual) {
            if (radius > 0) {
                node.visuals.emplace_back(RoundedRectVisual{radius, source.style.background});
            } else {
                node.visuals.emplace_back(RectVisual{source.style.background});
            }
        }
        if (!IsFloatingKind(source.kind) && !IsInteractionOwner(source.kind) &&
            source.interaction.hovered && snapshot.controls.hover.a) {
            node.visuals.emplace_back(RoundedRectVisual{radius, snapshot.controls.hover});
        }
        if (source.style.inner_shadow_color.a && source.style.inner_shadow_blur > 0) {
            node.visuals.emplace_back(ShadowVisual{radius, source.style.inner_shadow_blur,
                                                   source.style.inner_shadow_y,
                                                   source.style.inner_shadow_color, true});
        }
        if (source.style.border_width > 0 && source.style.border_color.a) {
            node.visuals.emplace_back(
                BorderVisual{radius, source.style.border_width, source.style.border_color});
        }
        if (!IsFloatingKind(source.kind) && !IsInteractionOwner(source.kind) &&
            source.interaction.focusVisible && snapshot.controls.focus_width > 0 &&
            snapshot.controls.focus.a) {
            node.visuals.emplace_back(
                BorderVisual{radius, snapshot.controls.focus_width, snapshot.controls.focus});
        }

        const bool editor = source.kind == Kind::TextField || source.kind == Kind::TextArea;
        if (editor) {
            auto selection = source.style.foreground;
            selection.a = 55;
            node.visuals.emplace_back(TextSelectionVisual{source.text_selection, selection});
            if (source.interaction.focused) {
                node.visuals.emplace_back(
                    TextSelectionVisual{{source.text_caret}, source.style.foreground});
            }
        }
        if ((source.kind == Kind::Text || editor) && !source.shaped.glyphs.empty()) {
            node.visuals.emplace_back(
                TextVisual{source.shaped, source.style.font_size, source.style.foreground});
        }
        if (source.kind == Kind::Image && source.image_ready) {
            node.visuals.emplace_back(ImageVisual{source.image, source.style.image_fit});
        }
        if ((source.kind == Kind::Icon || source.kind == Kind::IconButton) &&
            !source.icon.empty()) {
            constexpr std::string_view names[] = {
                "grid",           "music",       "settings",     "folder",    "terminal",
                "play",           "pause",       "previous",     "next",      "volume",
                "wifi",           "battery",     "search",       "sun",       "moon",
                "power",          "check",       "chevron",      "refresh",   "cpu",
                "memory",         "heart",       "layers",       "rectangle", "drop",
                "wifi-off",       "error",       "fullscreen",   "restore",   "split-horizontal",
                "split-vertical", "document",    "document-add", "save",      "close",
                "arrow-left",     "arrow-right", "trash",        "info",      "monitor",
                "brush",          "activity",    "clock",        "repeat",    "heart-outline"};
            const auto it = std::find(std::begin(names), std::end(names), source.icon);
            if (it != std::end(names)) {
                node.visuals.emplace_back(
                    IconVisual{static_cast<contracts::VectorIcon>(it - std::begin(names)),
                               source.style.foreground, source.style.padding});
            }
        }
        if (source.kind == Kind::Progress) {
            node.visuals.emplace_back(
                ProgressVisual{source.value, radius, source.style.foreground});
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
