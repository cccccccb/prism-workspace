#include "wlr_server_internal.hpp"

namespace prism::wm {
namespace {
bool VisibleInLayout(std::shared_ptr<tree::TreeNode> node)
{
    while (node) {
        const auto parent = node->GetParent();
        const auto container = std::dynamic_pointer_cast<tree::ContainerNode>(parent);
        if (container &&
            (container->GetLayoutMode() == tree::LayoutMode::Tabbed ||
             container->GetLayoutMode() == tree::LayoutMode::Stacked) &&
            container->GetActiveChild() != node) {
            return false;
        }
        node = parent;
    }
    return true;
}
} // namespace

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
    item->server->HandleXdgCommit(item);
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
        const auto output = PrimaryLogicalBounds();
        auto bounds = role == static_cast<int>(contracts::WindowRole::LayoutControls)
                          ? core::Rect{0, 0, 48, 48}
                          : theme_.ShellRect(role, int(output.width), int(output.height));
        bounds.x += output.x;
        bounds.y += output.y;
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
        if (!view->managed) {
            wl_client_post_implementation_error(
                wl_resource_get_client(view->toplevel->base->surface->resource),
                "Prism window capacity exceeded");
            return;
        }
        view->fullscreen = view->toplevel->requested.fullscreen;
        view->maximized = view->toplevel->requested.maximized;
        if (view->managed) {
            view->managed->SetFullscreen(view->fullscreen);
            view->managed->SetMinimumSize(view->toplevel->current.min_width,
                                          view->toplevel->current.min_height);
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
    if (surface_geometry_ && surface_geometry_->View() == view) {
        surface_geometry_.reset();
    }
    if (view->shell_role == static_cast<int>(contracts::WindowRole::LayoutControls) &&
        control_fade_) {
        control_fade_->Reset(surface_effects_.get());
    }

    layout_controls_.Revoke({view->instance});
    if (dragged_xdg_view_ == view) {
        dragged_xdg_view_ = nullptr;
    }
    view->mapped = false;
    view->visible = false;
    HandleShellUnavailable(view->shell_role);
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
    if (surface_geometry_ && surface_geometry_->View() == view) {
        surface_geometry_.reset();
    }
    layout_controls_.Revoke({view->instance});
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
        const auto role = view->shell_role;
        xdg_views_.erase(it);
        HandleShellUnavailable(role);
    }
    ArrangeXdgViews();
    SynchronizeXdgFocus();
}

void WlrServer::SetXdgFullscreen(WlrXdgView *view, bool enabled)
{
    if (!view || view->shell_role) {
        return;
    }
    if (view->fullscreen == enabled) {
        return;
    }
    auto motion = std::move(surface_geometry_);
    if (motion && motion->View() != view) {
        motion.reset();
    }
    wlr_box committed{};
    wlr_xdg_surface_get_geometry(view->toplevel->base, &committed);
    const auto from = motion ? motion->Submitted()
                             : contracts::LogicalRect{double(view->x), double(view->y),
                                                      double(std::max(1, committed.width)),
                                                      double(std::max(1, committed.height))};
    const auto from_style =
        motion ? motion->Decoration()
               : ResolveDecoration(theme_snapshot_.get(), view == focused_xdg_view_,
                                   view->fullscreen, false)
                     .style;
    if (motion) {
        motion->Restore();
        view->presentation.reset();
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
    const auto *spec = theme_snapshot_
                           ? contracts::FindMotion(theme_snapshot_->motion, "window.geometry")
                           : nullptr;
    if (spec && spec->duration_ms && view->mapped && view->visible && outputs_.size() == 1) {
        if (!motion) {
            motion = std::make_unique<SurfaceGeometry>(view);
        }
        CancelTouchesForSurface(view->toplevel->base->surface);
        motion->Start(from,
                      {double(view->x), double(view->y), double(view->width), double(view->height)},
                      from_style,
                      ResolveDecoration(theme_snapshot_.get(), view == focused_xdg_view_,
                                        view->fullscreen, false)
                          .style,
                      *spec);
        surface_geometry_ = std::move(motion);
        wlr_scene_node_raise_to_top(&view->scene_tree->node);
        ScheduleFrames(FrameReason::Layout);
    }
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
    if (surface_geometry_) {
        surface_geometry_->Restore();
    }
    if (!compositor_) {
        return;
    }
    const auto output = PrimaryLogicalBounds();
    const int width = static_cast<int>(output.width);
    const int height = static_cast<int>(output.height);
    compositor_->SetScreenSize(width, height);
    auto &engine = compositor_->GetTreeEngine();
    ReconcileGroupModes();
    const bool immersive = GroupImmersive();
    auto work_area =
        immersive ? core::Rect{0, 0, float(width), float(height)} : theme_.WorkArea(width, height);
    work_area.x += output.x;
    work_area.y += output.y;
    const auto config = CurrentTreeLayout();
    engine.Arrange(work_area, config);
    for (const auto &item : outputs_) {
        wlr_box box{};
        wlr_output_layout_get_box(output_layout_, item->wlr_output, &box);
        wlr_scene_output_set_position(item->scene_output, box.x, box.y);
    }
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
        if (view->shell_role == static_cast<int>(contracts::WindowRole::LayoutControls)) {
            continue;
        }
        core::Rect bounds{};
        if (view->shell_role) {
            bounds = theme_.ShellRect(view->shell_role, width, height);
            bounds.x += output.x;
            bounds.y += output.y;
            const bool recovery =
                (immersive || fullscreen) && recovery_visible_ &&
                view->shell_role == static_cast<int>(contracts::WindowRole::TopBar);
            view->visible =
                view->mapped && (view->shell_role == 1 || recovery || (!fullscreen && !immersive));
        } else if (view->managed) {
            auto node = engine.FindViewForWindow(view->managed);
            const bool in_workspace = node && node->GetWorkspace() == active;
            view->visible = view->mapped && in_workspace && VisibleInLayout(node) &&
                            (!fullscreen || fullscreen == view.get());
            if (fullscreen == view.get()) {
                bounds = output;
            } else if (node) {
                bounds = node->GetBounds();
            }
            view->managed->SetVisible(view->visible);
        } else {
            view->visible = false;
        }
        wlr_scene_node_set_enabled(&view->scene_tree->node, view->visible);
        if (!view->visible) {
            CancelLayoutControlsForSurface(view->toplevel->base->surface);
            CancelTouchesForSurface(view->toplevel->base->surface);
        }
        if (!view->shell_role && !view->managed) {
            continue;
        }
        const int x = static_cast<int>(std::round(bounds.x));
        const int y = static_cast<int>(std::round(bounds.y));
        const int w = std::max(1, static_cast<int>(std::round(bounds.width)));
        const int h = std::max(1, static_cast<int>(std::round(bounds.height)));
        view->x = x;
        view->y = y;
        if (view->managed) {
            // Target geometry reports the actual integer XDG configure and
            // scene position. The separate tree tile can retain fractional bounds.
            view->managed->SetBounds({float(x), float(y), float(w), float(h)});
        }
        wlr_scene_node_set_position(&view->scene_tree->node, x, y);
        if (view->shell_role == static_cast<int>(contracts::WindowRole::TopBar)) {
            wlr_box clip{};
            wlr_xdg_surface_get_geometry(view->toplevel->base, &clip);
            clip.width = w;
            clip.height = h;
            wlr_scene_subsurface_tree_set_clip(&view->scene_tree->node,
                                               recovery_visible_ ? &clip : nullptr);
        }
        // Pure topology swaps can move a view without asking the client to
        // resize or commit. Keep the committed size and displayed origin current.
        UpdateCommittedGeometry(view.get());
        if (view->width != w || view->height != h) {
            view->width = w;
            view->height = h;
            SubmitXdgSize(view.get());
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
    if (surface_geometry_ && !surface_geometry_->ValidTarget()) {
        surface_geometry_.reset();
    }
    InvalidateLayoutSnapshot();
    UpdateBoundaryControl();
    UpdateXdgPointerFocus(static_cast<uint32_t>(core::CurrentTimeNs() / 1000000));
    InvalidateEffects();
    ScheduleFrames(FrameReason::Layout);
    InvalidateLayoutSnapshot();
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
    InvalidateLayoutSnapshot();
}

void WlrServer::FocusXdgView(WlrXdgView *view)
{
    if (!view || !view->mapped || view->shell_role || !view->managed) {
        return;
    }
    if (surface_geometry_ && surface_geometry_->View() != view) {
        surface_geometry_.reset();
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
    InvalidateLayoutSnapshot();
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
