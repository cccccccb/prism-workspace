#include "prism/runtime/render_tree.hpp"
#include <stdexcept>

namespace prism::runtime {
namespace {
bool SameBounds(contracts::LogicalRect a, contracts::LogicalRect b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}
} // namespace

const RenderNode& RenderTree::Get(contracts::NodeId id) const {
    if (!id || id.index >= nodes.size() || nodes[id.index].id != id)
        throw std::out_of_range("Invalid render node");
    return nodes[id.index];
}

RenderTree RenderTreeBuilder::Build(const SceneSnapshot& snapshot, const RenderTree* previous) {
    RenderTree tree;
    tree.root = snapshot.root;
    tree.nodes.reserve(snapshot.nodes.size());
    for (const auto& source : snapshot.nodes) {
        const RenderNode* old = nullptr;
        if (previous && source.id.index < previous->nodes.size() &&
            previous->nodes[source.id.index].id == source.id)
            old = &previous->nodes[source.id.index];
        if (old && old->source_revision == source.revision &&
            SameBounds(old->bounds, source.bounds) && old->children == source.children) {
            tree.nodes.push_back(*old);
            continue;
        }
        RenderNode node;
        node.id = source.id;
        node.bounds = source.bounds;
        node.clip = source.style.clip;
        node.children = source.children;
        node.source_revision = source.revision;
        node.render_generation = old ? old->render_generation + 1 : 1;
        if (source.style.background.a) {
            if (source.style.radius > 0)
                node.visuals.emplace_back(RoundedRectVisual{source.style.radius, source.style.background});
            else node.visuals.emplace_back(RectVisual{source.style.background});
        }
        if (source.kind == Kind::Text && !source.shaped.glyphs.empty())
            node.visuals.emplace_back(TextVisual{source.shaped, source.style.font_size,
                                                source.style.foreground});
        if (source.kind == Kind::Image && source.image_ready)
            node.visuals.emplace_back(ImageVisual{source.image});
        tree.nodes.push_back(std::move(node));
    }
    return tree;
}
} // namespace prism::runtime
