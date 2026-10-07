#include "wlr_server_internal.hpp"

#include <new>
#include <stdexcept>

namespace prism::wm {
namespace {
constexpr std::size_t PopupLimit = 64;

wl_client *SurfaceClient(wlr_surface *surface)
{
    return surface && surface->resource ? wl_resource_get_client(surface->resource) : nullptr;
}

bool ParentReady(const WlrXdgPopup &popup)
{
    return popup.native && popup.native->parent && popup.native->parent->mapped && popup.owner &&
           popup.owner->mapped && popup.owner->visible && !popup.owner->presentation &&
           SurfaceClient(popup.native->parent) == SurfaceClient(popup.native->base->surface);
}
} // namespace

WlrXdgPopup::WlrXdgPopup()
{
    for (auto *listener :
         {&commit, &map, &unmap, &destroy, &parent_destroy, &reposition, &tree_destroy}) {
        wl_list_init(&listener->link);
    }
}

WlrXdgPopup::~WlrXdgPopup()
{
    if (dismiss_idle) {
        wl_event_source_remove(dismiss_idle);
    }
    for (auto *listener :
         {&commit, &map, &unmap, &destroy, &parent_destroy, &reposition, &tree_destroy}) {
        wl_list_remove(&listener->link);
    }
    if (tree) {
        wlr_scene_node_destroy(&tree->node);
    }
}

void handle_server_new_xdg_popup(wl_listener *listener, void *data)
{
    auto *signals =
        WlContainerOf<WlrServerSignals>(listener, offsetof(WlrServerSignals, new_xdg_popup));
    auto *native = static_cast<wlr_xdg_popup *>(data);
    auto *client = SurfaceClient(native->base->surface);
    try {
        signals->server->HandleNewXdgPopup(native);
    } catch (const std::bad_alloc &) {
        wl_client_post_no_memory(client);
    } catch (const std::exception &error) {
        wl_client_post_implementation_error(client, "Cannot create popup: %s", error.what());
    }
}

WlrXdgPopup *WlrServer::FindXdgPopup(wlr_surface *surface) const
{
    if (!surface) {
        return nullptr;
    }
    auto *root = wlr_surface_get_root_surface(surface);
    const auto found =
        std::find_if(xdg_popups_.begin(), xdg_popups_.end(), [root](const auto &item) {
            return item->native && item->native->base->surface == root;
        });
    return found == xdg_popups_.end() ? nullptr : found->get();
}

WlrXdgView *WlrServer::XdgOwner(wlr_surface *surface) const
{
    if (!surface) {
        return nullptr;
    }
    if (const auto *popup = FindXdgPopup(surface)) {
        return popup->owner;
    }
    auto *root = wlr_surface_get_root_surface(surface);
    const auto found = std::find_if(xdg_views_.begin(), xdg_views_.end(), [root](const auto &view) {
        return view->toplevel && view->toplevel->base->surface == root;
    });
    return found == xdg_views_.end() ? nullptr : found->get();
}

void WlrServer::HandleNewXdgPopup(wlr_xdg_popup *native)
{
    if (!native) {
        return;
    }
    auto *owner = XdgOwner(native->parent);
    auto *parent_popup = FindXdgPopup(native->parent);
    auto *parent = parent_popup ? parent_popup->tree : owner ? owner->scene_tree : nullptr;
    if (xdg_popups_.size() >= PopupLimit) {
        wl_client_post_implementation_error(SurfaceClient(native->base->surface),
                                            "Popup capacity exceeded");
        return;
    }

    auto popup = std::make_unique<WlrXdgPopup>();
    popup->server = this;
    popup->native = native;
    popup->owner = owner;
    popup->commit.notify = WlrXdgPopup::Commit;
    popup->map.notify = WlrXdgPopup::Map;
    popup->unmap.notify = WlrXdgPopup::Unmap;
    popup->destroy.notify = WlrXdgPopup::Destroy;
    popup->parent_destroy.notify = WlrXdgPopup::ParentGone;
    popup->reposition.notify = WlrXdgPopup::Reposition;
    popup->tree_destroy.notify = WlrXdgPopup::TreeGone;
    wl_signal_add(&native->base->surface->events.commit, &popup->commit);
    wl_signal_add(&native->base->surface->events.map, &popup->map);
    wl_signal_add(&native->base->surface->events.unmap, &popup->unmap);
    wl_signal_add(&native->events.destroy, &popup->destroy);
    if (native->parent) {
        wl_signal_add(&native->parent->events.destroy, &popup->parent_destroy);
    }
    wl_signal_add(&native->events.reposition, &popup->reposition);
    auto *record = popup.get();
    xdg_popups_.push_back(std::move(popup));

    if (!parent || !ParentReady(*record) ||
        (surface_geometry_ && surface_geometry_->View() == owner) ||
        (group_geometry_ && group_geometry_->ForView(owner))) {
        DismissXdgPopup(record);
        return;
    }
    record->tree = wlr_scene_xdg_surface_create(parent, native->base);
    if (!record->tree) {
        throw std::bad_alloc();
    }
    wl_signal_add(&record->tree->node.events.destroy, &record->tree_destroy);
}

void WlrServer::DismissXdgPopup(WlrXdgPopup *popup)
{
    if (!popup || popup->dismiss_idle) {
        return;
    }
    if (popup->tree) {
        wlr_scene_node_set_enabled(&popup->tree->node, false);
    }
    popup->dismiss_idle = wl_event_loop_add_idle(wl_event_loop_, WlrXdgPopup::Dismiss, popup);
    if (!popup->dismiss_idle) {
        wl_client_post_no_memory(SurfaceClient(popup->native->base->surface));
    }
}

void WlrXdgPopup::Dismiss(void *data)
{
    auto *popup = static_cast<WlrXdgPopup *>(data);
    popup->dismiss_idle = nullptr;
    wlr_xdg_popup_destroy(popup->native);
}

void WlrServer::ConfigureXdgPopup(WlrXdgPopup *popup)
{
    if (!popup || popup->dismiss_idle || !ParentReady(*popup) ||
        (popup->native->seat && popup->native->seat != seat_) ||
        (surface_geometry_ && surface_geometry_->View() == popup->owner) ||
        (group_geometry_ && group_geometry_->ForView(popup->owner))) {
        DismissXdgPopup(popup);
        return;
    }

    auto *parent_popup = FindXdgPopup(popup->native->parent);
    auto *parent = parent_popup ? parent_popup->tree : popup->owner->scene_tree;
    int x{}, y{};
    if (!parent || !wlr_scene_node_coords(&parent->node, &x, &y)) {
        DismissXdgPopup(popup);
        return;
    }
    wlr_box output{};
    auto *target =
        wlr_output_layout_output_at(output_layout_, popup->owner->x + popup->owner->width / 2.0,
                                    popup->owner->y + popup->owner->height / 2.0);
    if (!target || !target->enabled) {
        const auto enabled = std::find_if(outputs_.begin(), outputs_.end(), [](const auto &item) {
            return item->wlr_output->enabled;
        });
        if (enabled == outputs_.end()) {
            DismissXdgPopup(popup);
            return;
        }
        target = (*enabled)->wlr_output;
    }
    wlr_output_layout_get_box(output_layout_, target, &output);
    if (output.width <= 0 || output.height <= 0) {
        DismissXdgPopup(popup);
        return;
    }

    // scheduled.geometry is parent window-geometry-local. The wlroots scene
    // adapter already compensates both explicit and effective geometry origins.
    // Use that exact parent origin for nested popups as well as toplevels.
    const wlr_box constraint{output.x - x, output.y - y, output.width, output.height};
    wlr_xdg_positioner_rules_unconstrain_box(&popup->native->scheduled.rules, &constraint,
                                             &popup->native->scheduled.geometry);
    wlr_xdg_surface_schedule_configure(popup->native->base);
}

void WlrXdgPopup::Commit(wl_listener *listener, void *)
{
    auto *popup = WlContainerOf<WlrXdgPopup>(listener, offsetof(WlrXdgPopup, commit));
    if (popup->native->base->initial_commit) {
        popup->server->ConfigureXdgPopup(popup);
    }
}

void WlrXdgPopup::Reposition(wl_listener *listener, void *)
{
    auto *popup = WlContainerOf<WlrXdgPopup>(listener, offsetof(WlrXdgPopup, reposition));
    popup->server->ConfigureXdgPopup(popup);
}

void WlrXdgPopup::Map(wl_listener *listener, void *)
{
    auto *popup = WlContainerOf<WlrXdgPopup>(listener, offsetof(WlrXdgPopup, map));
    if (popup->dismiss_idle || !popup->tree || !ParentReady(*popup) ||
        (popup->native->seat && popup->native->seat != popup->server->seat_)) {
        popup->server->DismissXdgPopup(popup);
        return;
    }
    wlr_scene_node_set_enabled(&popup->tree->node, true);
    wlr_scene_node_raise_to_top(&popup->tree->node);
    popup->server->RestorePopupFocus();
    popup->server->InvalidateEffects();
}

void WlrXdgPopup::Unmap(wl_listener *listener, void *)
{
    auto *popup = WlContainerOf<WlrXdgPopup>(listener, offsetof(WlrXdgPopup, unmap));
    if (popup->tree) {
        wlr_scene_node_set_enabled(&popup->tree->node, false);
    }
    for (const auto &child : popup->server->xdg_popups_) {
        if (child->native->parent == popup->native->base->surface) {
            popup->server->DismissXdgPopup(child.get());
        }
    }
    popup->server->RestorePopupFocus();
    popup->server->InvalidateEffects();
}

void WlrXdgPopup::ParentGone(wl_listener *listener, void *)
{
    auto *popup = WlContainerOf<WlrXdgPopup>(listener, offsetof(WlrXdgPopup, parent_destroy));
    wlr_xdg_popup_destroy(popup->native);
}

void WlrXdgPopup::TreeGone(wl_listener *listener, void *)
{
    auto *popup = WlContainerOf<WlrXdgPopup>(listener, offsetof(WlrXdgPopup, tree_destroy));
    popup->tree = nullptr;
    wl_list_remove(&popup->tree_destroy.link);
    wl_list_init(&popup->tree_destroy.link);
}

void WlrXdgPopup::Destroy(wl_listener *listener, void *)
{
    auto *popup = WlContainerOf<WlrXdgPopup>(listener, offsetof(WlrXdgPopup, destroy));
    popup->server->RemoveXdgPopup(popup);
}

void WlrServer::RemoveXdgPopup(WlrXdgPopup *popup)
{
    auto *surface = popup->native->base->surface;
    if (seat_ && seat_->keyboard_state.focused_surface &&
        wlr_surface_get_root_surface(seat_->keyboard_state.focused_surface) == surface) {
        wlr_seat_keyboard_clear_focus(seat_);
    }
    if (seat_ && seat_->pointer_state.focused_surface &&
        wlr_surface_get_root_surface(seat_->pointer_state.focused_surface) == surface) {
        wlr_seat_pointer_notify_clear_focus(seat_);
    }
    const auto found = std::find_if(xdg_popups_.begin(), xdg_popups_.end(),
                                    [popup](const auto &item) { return item.get() == popup; });
    if (found != xdg_popups_.end()) {
        xdg_popups_.erase(found);
    }
    RestorePopupFocus();
    InvalidateEffects();
}

void WlrServer::CloseXdgPopups(WlrXdgView *owner)
{
    while (true) {
        const auto found =
            std::find_if(xdg_popups_.rbegin(), xdg_popups_.rend(),
                         [owner](const auto &popup) { return !owner || popup->owner == owner; });
        if (found == xdg_popups_.rend()) {
            return;
        }
        // Destruction emits callbacks which erase this and descendant records.
        // Retain no vector iterator or record reference across the protocol call.
        auto *native = (*found)->native;
        wlr_xdg_popup_destroy(native);
    }
}

void WlrServer::RestorePopupFocus()
{
    if (!seat_) {
        return;
    }
    auto *keyboard = wlr_seat_get_keyboard(seat_);
    if (!keyboard) {
        return;
    }
    for (auto it = xdg_popups_.rbegin(); it != xdg_popups_.rend(); ++it) {
        auto *popup = it->get();
        int x{}, y{};
        if (popup->native && popup->native->base->surface->mapped && !popup->dismiss_idle &&
            popup->tree && wlr_scene_node_coords(&popup->tree->node, &x, &y) &&
            ParentReady(*popup) && popup->native->seat == seat_) {
            // The XDG keyboard grab intentionally ignores notify_enter. This
            // explicit enter selects its mapped topmost popup without ending it.
            wlr_seat_keyboard_enter(seat_, popup->native->base->surface, keyboard->keycodes,
                                    keyboard->num_keycodes, &keyboard->modifiers);
            return;
        }
    }
    auto *focused = seat_->keyboard_state.focused_surface;
    if (FindXdgPopup(focused) || !focused) {
        if (focused_xdg_view_ && focused_xdg_view_->mapped && focused_xdg_view_->visible &&
            focused_xdg_view_->toplevel->base->surface->mapped) {
            wlr_seat_keyboard_notify_enter(seat_, focused_xdg_view_->toplevel->base->surface,
                                           keyboard->keycodes, keyboard->num_keycodes,
                                           &keyboard->modifiers);
        }
    }
}

} // namespace prism::wm
