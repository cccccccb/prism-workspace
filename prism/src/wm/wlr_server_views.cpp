#include "wlr_server_internal.hpp"

namespace prism::wm {
static void handle_xdg_set_app_id(wl_listener *listener, void *)
{
    auto *item = WlContainerOf<WlrXdgView>(listener, offsetof(WlrXdgView, set_app_id));
    if (item->managed) {
        item->managed->UpdateIdentity(item->toplevel->app_id ? item->toplevel->app_id : "",
                                      item->toplevel->title ? item->toplevel->title : "", item->pid,
                                      item->instance);
    }
}

static void handle_xdg_set_title(wl_listener *listener, void *)
{
    auto *item = WlContainerOf<WlrXdgView>(listener, offsetof(WlrXdgView, set_title));
    if (item->managed) {
        item->managed->UpdateIdentity(item->toplevel->app_id ? item->toplevel->app_id : "",
                                      item->toplevel->title ? item->toplevel->title : "", item->pid,
                                      item->instance);
    }
}

static void handle_xdg_commit(wl_listener *listener, void *)
{
    auto *item = WlContainerOf<WlrXdgView>(listener, offsetof(WlrXdgView, commit));
    if (item->toplevel->base->initial_commit) {
        wlr_xdg_toplevel_set_size(item->toplevel, item->width, item->height);
    }
    UpdateCommittedGeometry(item);
    // The all-surface observer compares the full local XDG geometry,
    // including x/y and Shell clients without a managed Window.
}

void WlrServer::HandleNewXdgToplevel(struct wlr_xdg_toplevel *toplevel)
{
    if (!toplevel || !windows_tree_) {
        return;
    }
    auto *client = wl_resource_get_client(toplevel->base->surface->resource);
    pid_t peer = -1;
    uid_t uid{};
    wl_client_get_credentials(client, &peer, &uid, nullptr);
    std::uint64_t instance{};
    int role{};
    if (auto found = registrations_.find(peer);
        found != registrations_.end() && !found->second->consumed) {
        auto &r = *found->second;
        pollfd dead{r.pidfd, POLLIN, 0};
        if (uid != geteuid() || poll(&dead, 1, 0) != 0 ||
            r.permit.expires_ns <= launch::MonotonicNs() ||
            (r.guard && !r.guard->Consume(r.permit, launch::MonotonicNs()))) {
            // A registered launch must fail if its identity expires; silently
            // mapping it as an ordinary window would conceal a Shell failure.
            wl_client_post_implementation_error(client,
                                                "Prism launch registration expired or invalid");
            return;
        }
        r.consumed = true;
        instance = r.permit.instance.value;
        role = static_cast<int>(r.permit.role);
    }
    auto view = std::make_unique<WlrXdgView>();
    view->server = this;
    view->toplevel = toplevel;
    view->instance = instance;
    view->pid = peer;
    view->shell_role = role;
    view->x = 0;
    view->y = 0;
    if (role) {
        const int width =
            !outputs_.empty() && outputs_[0]->wlr_output ? outputs_[0]->wlr_output->width : 1280;
        const int height =
            !outputs_.empty() && outputs_[0]->wlr_output ? outputs_[0]->wlr_output->height : 720;
        const auto bounds = theme_.ShellRect(role, width, height);
        view->x = static_cast<int>(bounds.x);
        view->y = static_cast<int>(bounds.y);
        view->width = static_cast<int>(bounds.width);
        view->height = static_cast<int>(bounds.height);
    }
    auto *parent = view->shell_role == 1  ? background_tree_
                   : view->shell_role > 1 ? chrome_tree_
                                          : windows_tree_;
    view->scene_tree = wlr_scene_xdg_surface_create(parent, toplevel->base);
    if (!view->scene_tree) {
        return;
    }
    wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
    wlr_scene_node_set_enabled(&view->scene_tree->node, false);
    view->map.notify = handle_xdg_map;
    view->unmap.notify = handle_xdg_unmap;
    view->destroy.notify = handle_xdg_destroy;
    view->request_maximize.notify = handle_xdg_maximize;
    view->request_fullscreen.notify = handle_xdg_fullscreen;
    wl_signal_add(&toplevel->base->surface->events.map, &view->map);
    wl_signal_add(&toplevel->base->surface->events.unmap, &view->unmap);
    wl_signal_add(&toplevel->events.destroy, &view->destroy);
    wl_signal_add(&toplevel->events.request_maximize, &view->request_maximize);
    wl_signal_add(&toplevel->events.request_fullscreen, &view->request_fullscreen);
    view->commit.notify = handle_xdg_commit;
    wl_signal_add(&toplevel->base->surface->events.commit, &view->commit);
    view->set_title.notify = handle_xdg_set_title;
    view->set_app_id.notify = handle_xdg_set_app_id;
    wl_signal_add(&toplevel->events.set_title, &view->set_title);
    wl_signal_add(&toplevel->events.set_app_id, &view->set_app_id);
    PRISM_LOG_INFO("WLR-XDG", "New XDG toplevel registered: title='%s' app_id='%s'",
                   toplevel->title ? toplevel->title : "(untitled)",
                   toplevel->app_id ? toplevel->app_id : "(none)");
    PRISM_LOG_INFO("WLR-XDG", "Client pid=%d authorized shell role=%d", peer, view->shell_role);
    xdg_views_.push_back(std::move(view));
}

void WlrServer::HandleXdgMap(WlrXdgView *view)
{
    view->mapped = true;
    if (!view->shell_role && compositor_) {
        view->managed = compositor_->ManageNativeWindow(
            view->toplevel->app_id ? view->toplevel->app_id : "",
            view->toplevel->title ? view->toplevel->title : "", view->pid, view->instance);
        view->fullscreen = view->toplevel->requested.fullscreen;
        view->maximized = view->toplevel->requested.maximized;
        if (view->managed) {
            view->managed->SetFullscreen(view->fullscreen);
        }
    }
    NotifyView(view, launch::ControlType::Mapped);
    ArrangeXdgViews();
    if (!view->shell_role) {
        FocusXdgView(view);
    } else {
        SynchronizeXdgFocus();
    }
    PRISM_LOG_INFO("WLR-XDG", "Mapped app_id='%s' shell role=%d pid=%d",
                   view->toplevel->app_id ? view->toplevel->app_id : "", view->shell_role,
                   view->pid);
}

void WlrServer::HandleXdgUnmap(WlrXdgView *view)
{
    if (dragged_xdg_view_ == view) {
        dragged_xdg_view_ = nullptr;
    }
    view->mapped = false;
    view->visible = false;
    NotifyView(view, launch::ControlType::Unmapped);
    wlr_scene_node_set_enabled(&view->scene_tree->node, false);
    if (view->managed && compositor_) {
        compositor_->DestroyWindow(view->managed);
        view->managed.reset();
    }
    if (focused_xdg_view_ == view) {
        focused_xdg_view_ = nullptr;
        wlr_seat_keyboard_notify_clear_focus(seat_);
    }
    if (seat_->pointer_state.focused_surface == view->toplevel->base->surface) {
        wlr_seat_pointer_notify_clear_focus(seat_);
    }
    ArrangeXdgViews();
    SynchronizeXdgFocus();
}

void WlrServer::HandleXdgDestroy(WlrXdgView *view)
{
    if (dragged_xdg_view_ == view) {
        dragged_xdg_view_ = nullptr;
    }
    if (view->managed && compositor_) {
        compositor_->DestroyWindow(view->managed);
        view->managed.reset();
    }
    if (focused_xdg_view_ == view) {
        focused_xdg_view_ = nullptr;
        wlr_seat_keyboard_notify_clear_focus(seat_);
    }
    auto it = std::find_if(xdg_views_.begin(), xdg_views_.end(),
                           [view](const auto &item) { return item.get() == view; });
    if (it != xdg_views_.end()) {
        xdg_views_.erase(it);
    }
    ArrangeXdgViews();
    SynchronizeXdgFocus();
}

void WlrServer::SetXdgFullscreen(WlrXdgView *view, bool enabled)
{
    if (!view || view->shell_role) {
        return;
    }
    view->fullscreen = enabled;
    if (view->managed) {
        view->managed->SetFullscreen(enabled);
    }
    wlr_xdg_toplevel_set_fullscreen(view->toplevel, enabled);
    if (enabled) {
        FocusXdgView(view);
    }
    ArrangeXdgViews();
}

void WlrServer::HandleXdgMaximize(WlrXdgView *view)
{
    if (!view || !view->toplevel || view->shell_role) {
        return;
    }
    view->maximized = view->toplevel->requested.maximized;
    wlr_xdg_toplevel_set_maximized(view->toplevel, view->maximized);
    // Maximization keeps the existing tile: tiled clients cannot overlap their
    // neighbours. Explicit fullscreen is a separate, reversible workspace mode.
    SetXdgFullscreen(view, view->toplevel->requested.fullscreen);
}

void WlrServer::ArrangeXdgViews()
{
    if (!compositor_) {
        return;
    }
    const int width =
        !outputs_.empty() && outputs_[0]->wlr_output ? outputs_[0]->wlr_output->width : 1280;
    const int height =
        !outputs_.empty() && outputs_[0]->wlr_output ? outputs_[0]->wlr_output->height : 720;
    compositor_->SetScreenSize(width, height);
    auto &engine = compositor_->GetTreeEngine();
    engine.Arrange(theme_.WorkArea(width, height), theme_.TreeLayout());
    const auto active = engine.GetActiveWorkspace();
    WlrXdgView *fullscreen = nullptr;
    for (const auto &view : xdg_views_) {
        if (!view->mapped || !view->managed) {
            continue;
        }
        auto node = engine.FindViewForWindow(view->managed);
        if (node && node->GetWorkspace() == active && view->fullscreen) {
            fullscreen = view.get();
            if (view.get() == focused_xdg_view_) {
                break;
            }
        }
    }
    for (auto &view : xdg_views_) {
        core::Rect bounds{};
        if (view->shell_role) {
            bounds = theme_.ShellRect(view->shell_role, width, height);
            view->visible = view->mapped && (!fullscreen || view->shell_role == 1);
        } else if (view->managed) {
            auto node = engine.FindViewForWindow(view->managed);
            const bool in_workspace = node && node->GetWorkspace() == active;
            view->visible =
                view->mapped && in_workspace && (!fullscreen || fullscreen == view.get());
            if (fullscreen == view.get()) {
                bounds = {0, 0, static_cast<float>(width), static_cast<float>(height)};
            } else if (node) {
                bounds = node->bounds;
            }
            view->managed->SetBounds(bounds);
            view->managed->SetVisible(view->visible);
        } else {
            view->visible = false;
        }
        wlr_scene_node_set_enabled(&view->scene_tree->node, view->visible);
        if (!view->shell_role && !view->managed) {
            continue;
        }
        const int x = static_cast<int>(std::round(bounds.x));
        const int y = static_cast<int>(std::round(bounds.y));
        const int w = std::max(1, static_cast<int>(std::round(bounds.width)));
        const int h = std::max(1, static_cast<int>(std::round(bounds.height)));
        view->x = x;
        view->y = y;
        wlr_scene_node_set_position(&view->scene_tree->node, x, y);
        // Pure topology swaps can move a view without asking the client to
        // resize or commit. Keep the committed size and displayed origin current.
        UpdateCommittedGeometry(view.get());
        if (view->width != w || view->height != h) {
            view->width = w;
            view->height = h;
            wlr_xdg_toplevel_set_size(view->toplevel, w, h);
        }
        if (!view->shell_role) {
            const std::uint32_t edges =
                view->fullscreen ? 0
                                 : WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT;
            if (edges != view->tiled_edges) {
                view->tiled_edges = edges;
                wlr_xdg_toplevel_set_tiled(view->toplevel, edges);
            }
        }
    }
    UpdateXdgPointerFocus(static_cast<uint32_t>(core::CurrentTimeNs() / 1000000));
    InvalidateEffects();
    ScheduleFrames(FrameReason::Layout);
}

void WlrServer::SynchronizeXdgFocus()
{
    if (!compositor_) {
        return;
    }
    compositor_->SynchronizeFocus();
    const auto win = compositor_->GetTreeEngine().GetFocusedWindow();
    for (auto &view : xdg_views_) {
        if (win && view->managed == win && view->visible) {
            FocusXdgView(view.get());
            return;
        }
    }
    if (focused_xdg_view_ && focused_xdg_view_->toplevel) {
        wlr_xdg_toplevel_set_activated(focused_xdg_view_->toplevel, false);
    }
    focused_xdg_view_ = nullptr;
    wlr_seat_keyboard_notify_clear_focus(seat_);
    InvalidateEffects();
}

void WlrServer::FocusXdgView(WlrXdgView *view)
{
    if (!view || !view->mapped || view->shell_role || !view->managed) {
        return;
    }
    const auto node = compositor_->GetTreeEngine().FindViewForWindow(view->managed);
    if (!node) {
        return;
    }
    // Activating an existing instance switches to its actual workspace.
    bool rearrange = node->GetWorkspace() != compositor_->GetTreeEngine().GetActiveWorkspace();
    for (auto &other : xdg_views_) {
        if (other.get() == view || !other->managed || !other->fullscreen) {
            continue;
        }
        const auto other_node = compositor_->GetTreeEngine().FindViewForWindow(other->managed);
        if (other_node && other_node->GetWorkspace() == node->GetWorkspace()) {
            other->fullscreen = false;
            other->managed->SetFullscreen(false);
            wlr_xdg_toplevel_set_fullscreen(other->toplevel, false);
            rearrange = true;
        }
    }
    compositor_->GetTreeEngine().SetFocusedWindow(view->managed);
    compositor_->SynchronizeFocus();
    if (rearrange) {
        ArrangeXdgViews();
    }
    if (!view->visible) {
        return;
    }
    const bool changed = focused_xdg_view_ != view;
    if (focused_xdg_view_ && changed && focused_xdg_view_->toplevel) {
        wlr_xdg_toplevel_set_activated(focused_xdg_view_->toplevel, false);
    }
    focused_xdg_view_ = view;
    wlr_scene_node_raise_to_top(&view->scene_tree->node);
    if (changed) {
        InvalidateEffects();
    }
    if (changed) {
        wlr_xdg_toplevel_set_activated(view->toplevel, true);
    }
    if (auto *keyboard = wlr_seat_get_keyboard(seat_);
        keyboard &&
        (changed || seat_->keyboard_state.focused_surface != view->toplevel->base->surface)) {
        wlr_seat_keyboard_notify_enter(seat_, view->toplevel->base->surface, keyboard->keycodes,
                                       keyboard->num_keycodes, &keyboard->modifiers);
    }
}

} // namespace prism::wm
