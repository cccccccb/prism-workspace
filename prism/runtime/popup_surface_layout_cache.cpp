#include "popup_surface_p.hpp"

#include <algorithm>
#include <tuple>

namespace prism::runtime {
namespace {
bool SameLayoutStyle(const Style &a, const Style &b)
{
    return std::tie(a.visible, a.width, a.height, a.padding, a.spacing, a.align, a.justify,
                    a.anchor, a.flex, a.inset, a.padding_x, a.padding_y, a.font_size,
                    a.line_height) == std::tie(b.visible, b.width, b.height, b.padding, b.spacing,
                                               b.align, b.justify, b.anchor, b.flex, b.inset,
                                               b.padding_x, b.padding_y, b.font_size,
                                               b.line_height);
}

bool SameLayoutSubtree(const SceneSnapshot &next, const SceneSnapshot &previous,
                       contracts::NodeId id)
{
    const auto &a = next.Get(id);
    const auto &b = previous.Get(id);
    if (a.id != b.id || a.kind != b.kind || a.parent != b.parent || a.children != b.children ||
        !SameLayoutStyle(a.style, b.style) || (a.kind == Kind::Text && a.text != b.text) ||
        (a.kind == Kind::Image &&
         (a.image_ready != b.image_ready || a.intrinsic_size != b.intrinsic_size))) {
        return false;
    }
    for (auto child : a.children) {
        if (!SameLayoutSubtree(next, previous, child)) {
            return false;
        }
    }
    return true;
}

void CopyLayout(SceneSnapshot &next, const SceneSnapshot &layout, contracts::NodeId id)
{
    auto &node = next.Get(id);
    const auto &cached = layout.Get(id);
    node.bounds = cached.bounds;
    node.intrinsic_size = cached.intrinsic_size;
    node.shaped = cached.shaped;
    node.scroll_content_height = cached.scroll_content_height;
    for (auto child : node.children) {
        CopyLayout(next, layout, child);
    }
}

void TranslateFlow(SceneSnapshot &snapshot, contracts::NodeId id, double delta)
{
    auto &node = snapshot.Get(id);
    node.bounds.y += delta;
    for (auto child : node.children) {
        TranslateFlow(snapshot, child, delta);
    }
}

void ResolveScrollOffsets(SceneSnapshot &next, const SceneSnapshot &layout, contracts::NodeId id)
{
    auto &node = next.Get(id);
    if (node.kind == Kind::ScrollView) {
        node.scroll_offset =
            std::clamp(node.scroll_offset, 0.0,
                       std::max(0.0, node.scroll_content_height - node.bounds.height));
        const double delta = layout.Get(id).scroll_offset - node.scroll_offset;
        for (auto child : node.children) {
            if (next.Get(child).kind != Kind::Visual) {
                TranslateFlow(next, child, delta);
            }
        }
    }
    for (auto child : node.children) {
        ResolveScrollOffsets(next, layout, child);
    }
}
} // namespace

bool ReusePopupLayout(SceneSnapshot &snapshot, const PopupSurfaceRequest &request,
                      const PopupSurfaceConfigure &configure, contracts::LogicalRect body,
                      const PopupSurfacePrepared *previous)
{
    if (!previous || !previous->layout || !previous->values.request.source ||
        previous->values.request.scene != request.scene ||
        previous->values.request.popup_token != request.popup_token ||
        previous->values.request.active_node != request.active_node ||
        previous->values.body_bounds != body || previous->values.request.anchor != request.anchor ||
        previous->values.request.parent_window_geometry != request.parent_window_geometry ||
        previous->configure.parent_configure_generation != configure.parent_configure_generation ||
        previous->configure.configure_generation != configure.configure_generation ||
        previous->configure.window_bounds != configure.window_bounds ||
        !SameLayoutSubtree(snapshot, *previous->values.request.source->snapshot,
                           request.active_node)) {
        return false;
    }

    CopyLayout(snapshot, *previous->layout, request.active_node);
    ResolveScrollOffsets(snapshot, *previous->layout, request.active_node);
    return true;
}
} // namespace prism::runtime
