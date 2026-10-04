#include "wlr_server_internal.hpp"

namespace prism::wm {
namespace {
constexpr int ControlRole = static_cast<int>(contracts::WindowRole::LayoutControls);
constexpr double HandleSize = 48;

bool Near(const contracts::LogicalRect &rect, double x, double y)
{
    return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
}

void Revisions(contracts::LayoutControlResult &result, const contracts::LayoutSnapshot &snapshot)
{
    result.revision = snapshot.revision;
    result.topology_revision = snapshot.topology_revision;
    result.layout_revision = snapshot.layout_revision;
}
} // namespace

tree::TreeLayoutConfig WlrServer::CurrentTreeLayout() const
{
    auto config = theme_.TreeLayout();
    if (GroupImmersive()) {
        config.outer_gap = 0;
    }
    return config;
}

WlrXdgView *WlrServer::BoundaryControlView() const
{
    for (const auto &view : xdg_views_) {
        if (view->shell_role == ControlRole && view->mapped) {
            return view.get();
        }
    }
    return nullptr;
}

void WlrServer::SampleBoundaryPointer()
{
    if (boundary_pointer_ && !boundary_pointer_->released) {
        boundary_pointer_->position = {cursor_->x, cursor_->y};
    }
}

void WlrServer::UpdateBoundaryControl()
{
    auto *view = BoundaryControlView();
    if (!view || !compositor_ || !cursor_) {
        if (boundary_handle_.visible) {
            boundary_handle_ = {};
            InvalidateLayoutSnapshot();
        }
        return;
    }

    if (UpdateWindowControl()) {
        return;
    }
    const auto snapshot = GetLayoutSnapshot();
    const auto output = PrimaryLogicalBounds();
    const bool held =
        boundary_drag_ || layout_controls_.HasPendingInput({view->instance}, launch::MonotonicNs());
    const bool supported = outputs_.size() == 1 && outputs_.front()->wlr_output->enabled;
    bool fullscreen = false;
    for (const auto &item : xdg_views_) {
        fullscreen |= item->mapped && item->visible && item->fullscreen && !item->shell_role;
    }

    contracts::LayoutControlHandle next;
    core::Rect gap{};
    for (const auto &boundary : snapshot->boundaries) {
        if (!supported || output.width < HandleSize || output.height < HandleSize || fullscreen ||
            !boundary.visible || !boundary.resizable ||
            (held && boundary.id != boundary_handle_.boundary)) {
            continue;
        }
        if (boundary.bounds.width < 1 || boundary.bounds.height < 1) {
            continue;
        }
        contracts::LogicalRect bounds{
            std::round(boundary.bounds.x + boundary.bounds.width / 2 - HandleSize / 2),
            std::round(boundary.bounds.y + boundary.bounds.height / 2 - HandleSize / 2), HandleSize,
            HandleSize};
        bounds.x =
            std::clamp(bounds.x, double(output.x), double(output.x + output.width) - HandleSize);
        bounds.y =
            std::clamp(bounds.y, double(output.y), double(output.y + output.height) - HandleSize);
        if (!held && !Near(bounds, cursor_->x, cursor_->y)) {
            continue;
        }
        next = {boundary.id, bounds, true};
        gap = {float(boundary.bounds.x), float(boundary.bounds.y), float(boundary.bounds.width),
               float(boundary.bounds.height)};
        break;
    }

    const bool changed = next != boundary_handle_;
    boundary_handle_ = next;
    if (view->width != 48 || view->height != 48) {
        view->width = view->height = 48;
        SubmitXdgSize(view);
    }
    view->visible = next.visible;
    wlr_scene_node_set_enabled(&view->scene_tree->node, view->visible);
    if (!next.visible) {
        if (changed) {
            CancelLayoutControlsForSurface(view->toplevel->base->surface);
        }
    } else {
        view->x = static_cast<int>(next.bounds.x);
        view->y = static_cast<int>(next.bounds.y);
        wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
        wlr_scene_node_raise_to_top(&view->scene_tree->node);
        wlr_box geometry{};
        wlr_xdg_surface_get_geometry(view->toplevel->base, &geometry);
        const int left = std::max(view->x, int(std::ceil(gap.x)));
        const int top = std::max(view->y, int(std::ceil(gap.y)));
        const int right = std::min(view->x + int(HandleSize), int(std::floor(gap.x + gap.width)));
        const int bottom = std::min(view->y + int(HandleSize), int(std::floor(gap.y + gap.height)));
        const wlr_box clip{left - view->x + geometry.x, top - view->y + geometry.y,
                           std::max(0, right - left), std::max(0, bottom - top)};
        // Enforce the actual gap even if a client submits a full input region.
        wlr_scene_subsurface_tree_set_clip(&view->scene_tree->node, &clip);
    }
    if (changed) {
        InvalidateLayoutSnapshot();
        ScheduleFrames(FrameReason::Layout);
    }
}

contracts::LayoutControlError
WlrServer::TrackLayoutIntent(const contracts::LayoutControlRequest &request,
                             contracts::LayoutControlResult &result)
{
    using enum contracts::LayoutControlError;
    using Phase = contracts::LayoutControlPhase;
    if (request.operation == contracts::LayoutControlOperation::WindowGesture) {
        return TrackWindowIntent(request);
    }
    if (request.operation != contracts::LayoutControlOperation::BoundaryGesture ||
        request.input.kind != contracts::LayoutInputKind::Pointer) {
        return Unsupported;
    }
    if (request.phase == Phase::Cancel) {
        CancelBoundaryPreview();
        Revisions(result, *GetLayoutSnapshot());
        return None;
    }
    if (!boundary_pointer_ || boundary_pointer_->proof != request.input ||
        boundary_pointer_->boundary != request.target.boundary) {
        return InvalidInput;
    }

    auto &engine = compositor_->GetTreeEngine();
    if (request.phase == Phase::Begin) {
        const auto fractions = engine.CaptureBoundaryFractions(request.target.boundary);
        const auto range = engine.GetBoundaryRange(request.target.boundary, CurrentTreeLayout());
        if (boundary_drag_ || !fractions || !range || !range->feasible ||
            !boundary_handle_.visible || boundary_handle_.boundary != request.target.boundary) {
            return StaleTarget;
        }
        boundary_drag_ = BoundaryDrag{result.session, *fractions, *fractions};
        return None;
    }
    if (!boundary_drag_ || boundary_drag_->session != request.session) {
        return UnknownSession;
    }

    const auto &pointer = *boundary_pointer_;
    const bool horizontal = boundary_drag_->original.axis == tree::LayoutMode::SplitHorizontal;
    const double delta =
        horizontal ? pointer.position.x - pointer.start.x : pointer.position.y - pointer.start.y;
    if (!engine.ApplyBoundary(request.target.boundary, pointer.divider_position + delta,
                              CurrentTreeLayout())) {
        return StaleLayout;
    }
    const auto fractions = *engine.CaptureBoundaryFractions(request.target.boundary);
    const bool changed = fractions.before_fraction != boundary_drag_->last.before_fraction ||
                         fractions.after_fraction != boundary_drag_->last.after_fraction;
    boundary_drag_->last = fractions;
    if (changed) {
        ArrangeXdgViews();
    }
    Revisions(result, *GetLayoutSnapshot());
    return None;
}

void WlrServer::CancelBoundaryPreview()
{
    if (!boundary_drag_) {
        return;
    }
    const auto saved = *boundary_drag_;
    boundary_drag_.reset();
    if (compositor_->GetTreeEngine().RestoreBoundaryFractions(saved.original, saved.last)) {
        ArrangeXdgViews();
    }
}

contracts::LayoutControlError
WlrServer::ApplyBoundaryIntent(const contracts::LayoutControlRequest &request,
                               contracts::LayoutControlResult &result)
{
    using enum contracts::LayoutControlError;
    if (request.intent != contracts::LayoutControlIntent::ApplyBoundary) {
        CancelBoundaryPreview();
        return Unsupported;
    }
    const auto error = TrackLayoutIntent(request, result);
    if (error != None) {
        CancelBoundaryPreview();
        Revisions(result, *GetLayoutSnapshot());
        return error;
    }
    boundary_drag_.reset();
    result.applied = true;
    return None;
}

} // namespace prism::wm
