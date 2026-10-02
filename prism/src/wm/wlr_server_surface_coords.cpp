#include "wlr_server_internal.hpp"

namespace prism::wm {
namespace {

wlr_scene_node *FindSurfaceNode(wlr_scene_node *node, wlr_surface *surface)
{
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        auto *scene_surface = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
        return scene_surface && scene_surface->surface == surface ? node : nullptr;
    }
    if (node->type != WLR_SCENE_NODE_TREE) {
        return nullptr;
    }

    auto *tree = wlr_scene_tree_from_node(node);
    wlr_scene_node *child;
    wl_list_for_each(child, &tree->children, link)
    {
        if (auto *found = FindSurfaceNode(child, surface)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

bool SurfacePosition(wlr_scene *scene, wlr_surface *surface, double lx, double ly, double &sx,
                     double &sy)
{
    if (!surface || !surface->mapped) {
        return false;
    }
    auto *node = FindSurfaceNode(&scene->tree.node, surface);
    int x{}, y{};
    if (!node || !wlr_scene_node_coords(node, &x, &y)) {
        return false;
    }

    sx = lx - x;
    sy = ly - y;
    return true;
}

} // namespace prism::wm
