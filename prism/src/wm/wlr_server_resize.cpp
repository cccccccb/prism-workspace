#include "wlr_server_internal.hpp"

namespace prism::wm {

void WlrServer::SubmitXdgSize(WlrXdgView *view)
{
    if (view->size_configure ||
        (view->configured_width == view->width && view->configured_height == view->height)) {
        return;
    }
    view->size_configure = wlr_xdg_toplevel_set_size(view->toplevel, view->width, view->height);
    view->configured_width = view->width;
    view->configured_height = view->height;
}

void WlrServer::HandleXdgCommit(WlrXdgView *view)
{
    const auto *surface = view->toplevel->base;
    if (surface->initial_commit) {
        view->size_configure = 0;
        view->configured_width = 0;
        view->configured_height = 0;
    } else if (view->size_configure && static_cast<std::int32_t>(surface->current.configure_serial -
                                                                 view->size_configure) >= 0) {
        // Acknowledgement alone is insufficient: only a surface commit consumes
        // a size target. Until then keep just the newest desired width/height.
        view->size_configure = 0;
    }

    if (view->managed) {
        const auto &state = view->toplevel->current;
        if (view->managed->GetMinimumWidth() != state.min_width ||
            view->managed->GetMinimumHeight() != state.min_height) {
            view->managed->SetMinimumSize(state.min_width, state.min_height);
            ++layout_constraints_generation_;
            if (const auto *control = BoundaryControlView()) {
                layout_controls_.CancelInstance({control->instance},
                                                contracts::LayoutControlError::StaleLayout);
            }
            CancelBoundaryPreview();
            InvalidateLayoutSnapshot();
            ArrangeXdgViews();
        }
    }

    SubmitXdgSize(view);
    UpdateCommittedGeometry(view);
}

} // namespace prism::wm
