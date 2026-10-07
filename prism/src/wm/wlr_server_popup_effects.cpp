#include "wlr_server_internal.hpp"

namespace prism::wm {
std::vector<SurfaceEffects::Target> WlrServer::PopupEffectTargets() const
{
    std::vector<SurfaceEffects::Target> targets;
    targets.reserve(xdg_popups_.size());

    for (const auto &popup : xdg_popups_) {
        const auto *native = popup->native;
        const auto *owner = popup->owner;
        if (!native || !native->base || !native->base->surface || !native->base->surface->mapped ||
            !native->parent || !native->parent->mapped || !owner || !owner->mapped ||
            !owner->visible || owner->presentation || !popup->tree || popup->dismiss_idle) {
            continue;
        }

        int x{};
        int y{};
        if (!wlr_scene_node_coords(&popup->tree->node, &x, &y)) {
            continue;
        }

        targets.push_back({native->base->surface, popup->tree});
    }

    return targets;
}
} // namespace prism::wm
