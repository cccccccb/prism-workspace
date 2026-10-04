#include "wlr_server_internal.hpp"

namespace prism::wm {
namespace {
bool Contains(const contracts::LogicalRect &bounds, double x, double y)
{
    return x >= bounds.x && y >= bounds.y && x < bounds.x + bounds.width &&
           y < bounds.y + bounds.height;
}
} // namespace

WlrXdgView *WlrServer::WindowControlTarget() const
{
    for (const auto &view : xdg_views_) {
        if (view->managed && view->mapped && view->visible) {
            const auto node = compositor_->GetTreeEngine().FindViewForWindow(view->managed);
            if (node && node->GetNodeId() == window_control_.node) {
                return view.get();
            }
        }
    }
    return nullptr;
}

void WlrServer::CloseWindowControl(bool animate)
{
    if (!window_control_.node) {
        if (!animate && control_fade_) {
            control_fade_->Reset(surface_effects_.get());
        }
        return;
    }
    window_control_ = {};
    boundary_handle_ = {};
    if (auto *view = BoundaryControlView()) {
        if (control_fade_) {
            if (animate) {
                control_fade_->Close(view, surface_effects_.get());
            } else {
                control_fade_->Reset(surface_effects_.get());
            }
        }
        CancelLayoutControlsForSurface(view->toplevel->base->surface);
        view->visible = false;
        wlr_scene_node_set_enabled(&view->scene_tree->node, false);
    }
    InvalidateLayoutSnapshot();
    ScheduleFrames(FrameReason::Layout);
}

bool WlrServer::ConsumeWindowPointer(std::uint32_t button, std::uint32_t state,
                                     wlr_input_device *device)
{
    if (state != WLR_BUTTON_PRESSED) {
        return false;
    }
    if (window_control_.node && !Contains(window_control_.bounds, cursor_->x, cursor_->y)) {
        CloseWindowControl(true);
        recovery_buttons_.insert({device, button});
        return true;
    }
    auto *keyboard = wlr_seat_get_keyboard(seat_);
    auto *control = BoundaryControlView();
    if (button != 273 || !keyboard || !(wlr_keyboard_get_modifiers(keyboard) & WLR_MODIFIER_LOGO) ||
        !control || seat_->pointer_state.button_count || dragged_xdg_view_ ||
        wlr_seat_pointer_has_grab(seat_) || outputs_.size() != 1 ||
        !outputs_.front()->wlr_output->enabled) {
        return false;
    }
    UpdateXdgPointerFocus(static_cast<std::uint32_t>(launch::MonotonicNs() / 1000000));
    auto *surface = seat_->pointer_state.focused_surface;
    WlrXdgView *target{};
    for (const auto &view : xdg_views_) {
        if (view->managed && view->mapped && view->visible && surface &&
            wlr_surface_get_root_surface(surface) == view->toplevel->base->surface) {
            target = view.get();
            break;
        }
    }
    const auto output = PrimaryLogicalBounds();
    if (!target || output.width < 168 || output.height < 56) {
        return false;
    }

    if (window_control_.node) {
        CloseWindowControl();
    }
    layout_controls_.CancelInput(contracts::LayoutInputKind::Pointer, 0);
    CancelBoundaryPreview();
    FocusXdgView(target);
    const auto snapshot = GetLayoutSnapshot();
    const auto node = compositor_->GetTreeEngine().FindViewForWindow(target->managed);
    window_control_.node = node->GetNodeId();
    window_control_.device = device;
    window_control_.topology = snapshot->topology_revision;
    window_control_.layout = snapshot->layout_revision;
    window_control_.bounds = {std::round(std::clamp(cursor_->x - 84, double(output.x),
                                                    double(output.x + output.width - 168))),
                              std::round(std::clamp(cursor_->y - 28, double(output.y),
                                                    double(output.y + output.height - 56))),
                              168, 56};
    recovery_buttons_.insert({device, button});
    wlr_seat_pointer_notify_clear_focus(seat_);
    wlr_seat_pointer_notify_frame(seat_);
    UpdateWindowControl();
    return true;
}

bool WlrServer::UpdateWindowControl()
{
    if (!window_control_.node) {
        return false;
    }
    const auto snapshot = GetLayoutSnapshot();
    auto *view = BoundaryControlView();
    if (!view || !WindowControlTarget() ||
        snapshot->topology_revision != window_control_.topology ||
        snapshot->layout_revision != window_control_.layout) {
        CloseWindowControl();
        return false;
    }
    const auto &bounds = window_control_.bounds;
    const contracts::LayoutControlHandle next{0, bounds, true, window_control_.node};
    if (next == boundary_handle_) {
        return true;
    }
    boundary_handle_ = next;
    view->visible = true;
    view->x = int(bounds.x);
    view->y = int(bounds.y);
    view->width = int(bounds.width);
    view->height = int(bounds.height);
    SubmitXdgSize(view);
    wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
    wlr_scene_node_set_enabled(&view->scene_tree->node, true);
    wlr_scene_node_raise_to_top(&view->scene_tree->node);
    wlr_box geometry{};
    wlr_xdg_surface_get_geometry(view->toplevel->base, &geometry);
    geometry.width = view->width;
    geometry.height = view->height;
    wlr_scene_subsurface_tree_set_clip(&view->scene_tree->node, &geometry);
    if (theme_snapshot_) {
        if (const auto *spec = contracts::FindMotion(theme_snapshot_->motion, "panel.visibility")) {
            if (!control_fade_) {
                control_fade_ = std::make_unique<SurfaceFade>();
            }
            control_fade_->Open(view, surface_effects_.get(), *spec);
        }
    }
    InvalidateLayoutSnapshot();
    ScheduleFrames(FrameReason::Layout);
    return true;
}

contracts::LayoutControlError
WlrServer::TrackWindowIntent(const contracts::LayoutControlRequest &request) const
{
    using enum contracts::LayoutControlError;
    if (request.input.kind != contracts::LayoutInputKind::Pointer) {
        return Unsupported;
    }
    if (!window_control_.node || request.target.node != window_control_.node ||
        window_control_.proof_node != request.target.node ||
        window_control_.proof != request.input || !WindowControlTarget()) {
        return InvalidInput;
    }
    return None;
}

contracts::LayoutControlError
WlrServer::ApplyWindowIntent(const contracts::LayoutControlRequest &request,
                             contracts::LayoutControlResult &result)
{
    using enum contracts::LayoutControlError;
    using Intent = contracts::LayoutControlIntent;
    const auto error = TrackWindowIntent(request);
    if (error != None) {
        return error;
    }
    auto *view = WindowControlTarget();
    if (!window_control_.released ||
        !Contains(window_control_.bounds, window_control_.release.x, window_control_.release.y)) {
        return InvalidInput;
    }
    auto &engine = compositor_->GetTreeEngine();
    const auto node = engine.FindViewForWindow(view->managed);
    const auto parent = node->GetParentContainer();
    const bool split =
        request.intent == Intent::SplitHorizontal || request.intent == Intent::SplitVertical;
    if (split && (view->fullscreen || !parent || parent->GetChildren().size() < 2)) {
        return Unsupported;
    }

    CloseWindowControl(true);
    if (split) {
        engine.SetLayoutMode(parent, request.intent == Intent::SplitHorizontal
                                         ? tree::LayoutMode::SplitHorizontal
                                         : tree::LayoutMode::SplitVertical);
        ArrangeXdgViews();
    } else {
        SetXdgFullscreen(view, request.intent == Intent::EnterWindowFullscreen);
    }
    const auto snapshot = GetLayoutSnapshot();
    result.revision = snapshot->revision;
    result.topology_revision = snapshot->topology_revision;
    result.layout_revision = snapshot->layout_revision;
    result.applied = true;
    return None;
}
} // namespace prism::wm
