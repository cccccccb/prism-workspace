#include "wlr_server_internal.hpp"

#include <limits>
#include <stdexcept>

namespace prism::wm {
namespace {
void AdvanceMode(std::uint64_t &revision)
{
    if (revision == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("WM group mode revision exhausted");
    }
    ++revision;
}
} // namespace

void WlrServer::ReconcileGroupModes()
{
    const auto &engine = compositor_->GetTreeEngine();
    const auto &workspaces = engine.GetWorkspaces();
    const auto active = engine.GetActiveWorkspace();
    const auto workspace = active ? active->GetNodeId() : 0;
    const auto output = outputs_.size() == 1 && outputs_.front()->wlr_output->enabled
                            ? outputs_.front()->layout_id
                            : 0;
    const auto bounds = PrimaryLogicalBounds();
    if (workspace != recovery_workspace_ || output != recovery_output_ ||
        bounds.x != recovery_output_bounds_.x || bounds.y != recovery_output_bounds_.y ||
        bounds.width != recovery_output_bounds_.width ||
        bounds.height != recovery_output_bounds_.height) {
        recovery_visible_ = false;
    }
    recovery_workspace_ = workspace;
    recovery_output_ = output;
    recovery_output_bounds_ = bounds;

    for (const auto &item : workspaces) {
        auto &state = group_modes_[item->GetNodeId()];
        if (state.output != output || !output) {
            if (state.mode != contracts::LayoutGroupMode::Normal) {
                state.mode = contracts::LayoutGroupMode::Normal;
                AdvanceMode(state.revision);
            }
            state.output = output;
        }
    }
    std::erase_if(group_modes_, [&workspaces](const auto &entry) {
        return std::none_of(workspaces.begin(), workspaces.end(), [&entry](const auto &item) {
            return item->GetNodeId() == entry.first;
        });
    });
}

bool WlrServer::GroupImmersive() const
{
    if (!compositor_ || outputs_.size() != 1 || !outputs_.front()->wlr_output->enabled) {
        return false;
    }
    const auto active = compositor_->GetTreeEngine().GetActiveWorkspace();
    const auto found = active ? group_modes_.find(active->GetNodeId()) : group_modes_.end();
    return found != group_modes_.end() && found->second.output == outputs_.front()->layout_id &&
           found->second.mode == contracts::LayoutGroupMode::Immersive;
}

bool WlrServer::SetGroupMode(std::uint64_t workspace, contracts::LayoutGroupMode mode)
{
    const auto found = group_modes_.find(workspace);
    if (found == group_modes_.end() || !found->second.output) {
        return false;
    }
    AdvanceMode(found->second.revision);
    found->second.mode = mode;
    recovery_visible_ = false;
    InvalidateLayoutSnapshot();
    return true;
}

bool WlrServer::ClearGroupFullscreen(std::uint64_t workspace)
{
    bool changed = false;
    const auto &engine = compositor_->GetTreeEngine();
    for (const auto &view : xdg_views_) {
        if (!view->managed || !view->fullscreen) {
            continue;
        }
        const auto node = engine.FindViewForWindow(view->managed);
        const auto owner = node ? node->GetWorkspace() : nullptr;
        if (!owner || (workspace && owner->GetNodeId() != workspace)) {
            continue;
        }

        view->fullscreen = false;
        view->managed->SetFullscreen(false);
        wlr_xdg_toplevel_set_fullscreen(view->toplevel, false);
        changed = true;
    }
    return changed;
}

void WlrServer::RestoreDesktopGroup(bool all)
{
    if (!compositor_) {
        return;
    }
    ReconcileGroupModes();
    const auto active = compositor_->GetTreeEngine().GetActiveWorkspace();
    const auto workspace = active ? active->GetNodeId() : 0;
    if (!all && !workspace) {
        return;
    }

    bool changed = recovery_visible_;
    recovery_visible_ = false;
    for (auto &[id, state] : group_modes_) {
        if ((all || id == workspace) && state.mode != contracts::LayoutGroupMode::Normal) {
            state.mode = contracts::LayoutGroupMode::Normal;
            AdvanceMode(state.revision);
            changed = true;
        }
    }
    changed = ClearGroupFullscreen(all ? 0 : workspace) || changed;
    if (changed) {
        ArrangeXdgViews();
        SynchronizeXdgFocus();
    }
}

void WlrServer::HandleShellUnavailable(int role)
{
    if (role) {
        RestoreDesktopGroup(true);
    }
}

contracts::LayoutControlError
WlrServer::ApplyLayoutIntent(const contracts::LayoutControlRequest &request,
                             contracts::LayoutControlResult &result)
{
    using enum contracts::LayoutControlError;
    using Intent = contracts::LayoutControlIntent;
    if (request.operation != contracts::LayoutControlOperation::GroupGesture ||
        request.input.kind != contracts::LayoutInputKind::Pointer ||
        (request.intent != Intent::EnterImmersive && request.intent != Intent::ExitImmersive)) {
        return Unsupported;
    }

    const auto mode = request.intent == Intent::EnterImmersive
                          ? contracts::LayoutGroupMode::Immersive
                          : contracts::LayoutGroupMode::Normal;
    if (!SetGroupMode(request.target.workspace, mode)) {
        return StaleTarget;
    }
    if (request.intent == Intent::ExitImmersive) {
        ClearGroupFullscreen(request.target.workspace);
    }
    ArrangeXdgViews();
    SynchronizeXdgFocus();

    const auto snapshot = GetLayoutSnapshot();
    result.revision = snapshot->revision;
    result.topology_revision = snapshot->topology_revision;
    result.layout_revision = snapshot->layout_revision;
    result.applied = true;
    return None;
}

} // namespace prism::wm
