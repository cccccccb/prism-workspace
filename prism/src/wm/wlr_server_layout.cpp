#include "wlr_server_internal.hpp"
#include <limits>
#include <unordered_map>

namespace prism::wm {
namespace {

contracts::LogicalRect Rect(const core::Rect &value)
{
    return {value.x, value.y, value.width, value.height};
}

std::uint64_t NextRevision(std::uint64_t value)
{
    if (value == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("WM layout revision exhausted");
    }
    return value + 1;
}

contracts::LayoutArrangement Arrangement(tree::LayoutMode mode)
{
    switch (mode) {
    case tree::LayoutMode::None:
        return contracts::LayoutArrangement::None;
    case tree::LayoutMode::SplitHorizontal:
        return contracts::LayoutArrangement::Horizontal;
    case tree::LayoutMode::SplitVertical:
        return contracts::LayoutArrangement::Vertical;
    case tree::LayoutMode::Tabbed:
        return contracts::LayoutArrangement::Tabbed;
    case tree::LayoutMode::Stacked:
        return contracts::LayoutArrangement::Stacked;
    }
    throw std::logic_error("Unknown tree layout mode");
}

bool SameOutputIdentities(const contracts::LayoutSnapshot &a, const contracts::LayoutSnapshot &b)
{
    if (a.outputs.size() != b.outputs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.outputs.size(); ++i) {
        if (a.outputs[i].id != b.outputs[i].id) {
            return false;
        }
    }
    return true;
}

bool SameGeometry(const contracts::LayoutSnapshot &a, const contracts::LayoutSnapshot &b)
{
    if (a.outputs != b.outputs || a.workspaces != b.workspaces || a.boundaries != b.boundaries ||
        a.nodes.size() != b.nodes.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.nodes.size(); ++i) {
        const auto &x = a.nodes[i];
        const auto &y = b.nodes[i];
        if (x.id != y.id || x.tile_bounds != y.tile_bounds || x.target_bounds != y.target_bounds ||
            x.width_fraction != y.width_fraction || x.height_fraction != y.height_fraction ||
            x.visible != y.visible || x.fullscreen != y.fullscreen) {
            return false;
        }
    }
    return true;
}

std::uint64_t FocusedNode(const contracts::LayoutSnapshot &snapshot)
{
    for (const auto &node : snapshot.nodes) {
        if (node.focused) {
            return node.id;
        }
    }
    return 0;
}

void AppendNodes(contracts::LayoutSnapshot &result, const tree::TreeSnapshot &snapshot,
                 tree::TreeEngine &engine, const Window *focused, bool output_enabled)
{
    std::unordered_map<std::uint64_t, std::shared_ptr<Window>> windows;
    for (const auto &workspace : engine.GetWorkspaces()) {
        std::vector<std::shared_ptr<tree::TreeNode>> views;
        workspace->CollectViews(views);
        for (const auto &view : views) {
            windows.emplace(view->GetNodeId(), view->GetWindow());
        }
    }

    std::unordered_map<std::uint64_t, std::size_t> indices;
    for (const auto &source : snapshot.nodes) {
        if (source.type == tree::NodeType::Workspace) {
            continue;
        }
        contracts::LayoutNode node;
        node.id = source.id;
        node.parent = source.parent == source.workspace ? 0 : source.parent;
        node.workspace = source.workspace;
        node.kind = source.type == tree::NodeType::View ? contracts::LayoutNodeKind::View
                                                        : contracts::LayoutNodeKind::Container;
        node.layout = Arrangement(source.layout);
        node.children = source.children;
        node.tile_bounds = Rect(source.bounds);
        node.target_bounds = node.tile_bounds;
        node.width_fraction = source.width_fraction;
        node.height_fraction = source.height_fraction;
        node.visible = source.workspace == snapshot.active_workspace && output_enabled;
        if (const auto it = windows.find(node.id); it != windows.end() && it->second) {
            const auto &window = *it->second;
            node.instance = {window.GetInstance()};
            node.target_bounds = Rect(window.GetBounds());
            const auto committed = Rect(window.GetCommittedBounds());
            node.has_committed = committed.width > 0 && committed.height > 0;
            node.committed_bounds = node.has_committed ? committed : contracts::LogicalRect{};
            node.visible = node.visible && window.IsVisible();
            node.fullscreen = window.IsFullscreen();
            node.focused = node.visible && &window == focused;
            if (node.focused) {
                result.active_instance = node.instance;
            }
        }
        indices.emplace(node.id, result.nodes.size());
        result.nodes.push_back(std::move(node));
    }

    // A hidden or fullscreen-covered subtree must not advertise visible handles.
    for (auto it = result.nodes.rbegin(); it != result.nodes.rend(); ++it) {
        if (it->kind != contracts::LayoutNodeKind::Container || it->children.empty()) {
            continue;
        }
        bool any_visible = false;
        for (const auto child : it->children) {
            any_visible |= result.nodes.at(indices.at(child)).visible;
        }
        it->visible = it->visible && any_visible;
    }
    for (const auto &source : snapshot.boundaries) {
        contracts::LayoutBoundary boundary;
        boundary.id = source.id;
        boundary.parent = source.parent;
        boundary.first = source.before;
        boundary.second = source.after;
        boundary.workspace = source.workspace;
        boundary.axis = source.axis == tree::LayoutMode::SplitHorizontal
                            ? contracts::LayoutBoundaryAxis::X
                            : contracts::LayoutBoundaryAxis::Y;
        boundary.bounds = Rect(source.bounds);
        boundary.visible = result.nodes.at(indices.at(source.parent)).visible &&
                           result.nodes.at(indices.at(source.before)).visible &&
                           result.nodes.at(indices.at(source.after)).visible;
        result.boundaries.push_back(std::move(boundary));
    }
}
} // namespace

void WlrServer::InvalidateLayoutSnapshot() noexcept
{
    layout_snapshot_dirty_ = true;
}

core::Rect WlrServer::PrimaryLogicalBounds() const
{
    if (outputs_.empty()) {
        return {0, 0, 1280, 720};
    }
    wlr_box box{};
    wlr_output_layout_get_box(output_layout_, outputs_.front()->wlr_output, &box);
    return {float(box.x), float(box.y), float(box.width), float(box.height)};
}

void WlrServer::HandleOutputLayoutChange(wl_listener *listener, void *)
{
    auto *signals =
        WlContainerOf<WlrServerSignals>(listener, offsetof(WlrServerSignals, output_layout_change));
    signals->server->ArrangeXdgViews();
}

std::shared_ptr<const contracts::LayoutSnapshot> WlrServer::GetLayoutSnapshot()
{
    if (!compositor_) {
        return {};
    }
    auto &engine = compositor_->GetTreeEngine();
    const auto tree = engine.CaptureSnapshot();
    const auto theme_generation = theme_snapshot_ ? theme_snapshot_->generation : 0;
    if (layout_snapshot_ && !layout_snapshot_dirty_ && layout_tree_snapshot_ == tree &&
        layout_theme_generation_ == theme_generation &&
        layout_observed_constraints_generation_ == layout_constraints_generation_) {
        return layout_snapshot_;
    }

    contracts::LayoutSnapshot next;
    next.session = control_session_;
    for (const auto &output : outputs_) {
        wlr_box box{};
        wlr_output_layout_get_box(output_layout_, output->wlr_output, &box);
        const bool primary = output.get() == outputs_.front().get();
        next.outputs.push_back(
            {output->layout_id,
             contracts::LayoutDisplayName(output->wlr_output->name ? output->wlr_output->name : ""),
             {double(box.x), double(box.y), double(box.width), double(box.height)},
             output->wlr_output->scale,
             primary,
             primary && outputs_.size() == 1 && output->wlr_output->enabled});
    }
    for (const auto &workspace : tree->workspaces) {
        const auto state = group_modes_.find(workspace.id);
        const auto mode =
            state == group_modes_.end() ? contracts::LayoutGroupMode::Normal : state->second.mode;
        const auto mode_revision = state == group_modes_.end() ? 1 : state->second.revision;
        next.workspaces.push_back(
            {workspace.id, workspace.root,
             workspace.active && !next.outputs.empty() ? next.outputs.front().id : 0,
             contracts::LayoutDisplayName(workspace.name), workspace.active, mode, mode_revision});
    }
    AppendNodes(next, *tree, engine, focused_xdg_view_ ? focused_xdg_view_->managed.get() : nullptr,
                !outputs_.empty() && outputs_.front()->wlr_output->enabled);

    for (auto &boundary : next.boundaries) {
        const auto range = engine.GetBoundaryRange(boundary.id, CurrentTreeLayout());
        boundary.resizable = boundary.visible && range && range->feasible;
    }
    if (boundary_handle_.visible) {
        const auto selected = std::find_if(
            next.boundaries.begin(), next.boundaries.end(),
            [this](const auto &boundary) { return boundary.id == boundary_handle_.boundary; });
        const auto output = PrimaryLogicalBounds();
        const auto &bounds = boundary_handle_.bounds;
        const bool window =
            std::any_of(next.nodes.begin(), next.nodes.end(), [this](const auto &node) {
                return node.id == boundary_handle_.node &&
                       node.kind == contracts::LayoutNodeKind::View && node.visible;
            });
        if (((selected != next.boundaries.end() && selected->resizable) || window) &&
            next.outputs.size() == 1 && next.outputs.front().supported && bounds.x >= output.x &&
            bounds.y >= output.y && bounds.x + bounds.width <= output.x + output.width &&
            bounds.y + bounds.height <= output.y + output.height) {
            next.control_handle = boundary_handle_;
        }
    }

    const auto &old_tree = layout_tree_snapshot_;
    bool topology = !old_tree || old_tree->topology_revision != tree->topology_revision;
    bool layout = !old_tree || old_tree->layout_revision != tree->layout_revision ||
                  layout_theme_generation_ != theme_generation ||
                  layout_observed_constraints_generation_ != layout_constraints_generation_;
    bool focus = !old_tree || old_tree->focus_revision != tree->focus_revision ||
                 old_tree->active_workspace != tree->active_workspace;
    if (layout_snapshot_) {
        const auto &old = *layout_snapshot_;
        topology |= !SameOutputIdentities(next, old);
        layout |= !SameGeometry(next, old);
        focus |=
            FocusedNode(next) != FocusedNode(old) || next.active_instance != old.active_instance;
        next.revision = old.revision;
        next.topology_revision = old.topology_revision;
        next.layout_revision = old.layout_revision;
        next.focus_revision = old.focus_revision;
    }
    if (!layout_snapshot_ || topology || layout || focus || next != *layout_snapshot_) {
        next.revision = NextRevision(next.revision);
        next.topology_revision += topology;
        next.layout_revision += layout;
        next.focus_revision += focus;
        if (!next.topology_revision || !next.layout_revision || !next.focus_revision) {
            throw std::overflow_error("WM layout component revision exhausted");
        }
        contracts::ValidateLayoutSnapshot(next);
        layout_snapshot_ = std::make_shared<const contracts::LayoutSnapshot>(std::move(next));
    }
    layout_tree_snapshot_ = tree;
    layout_theme_generation_ = theme_generation;
    layout_observed_constraints_generation_ = layout_constraints_generation_;
    layout_snapshot_dirty_ = false;
    return layout_snapshot_;
}

void WlrServer::PublishLayoutSnapshot()
{
    if (!control_ || control_failed_) {
        return;
    }
    try {
        auto snapshot = GetLayoutSnapshot();
        layout_controls_.Reconcile(*snapshot, launch::MonotonicNs());
        PublishLayoutControlNotifications();
        snapshot = GetLayoutSnapshot();
        if (layout_subscribed_ && snapshot && snapshot->revision != layout_sent_revision_) {
            launch::ControlMessage message;
            message.type = launch::ControlType::LayoutSnapshot;
            message.permit.session = control_session_;
            message.layout_snapshot = *snapshot;
            if (!control_->Queue(launch::EncodeControl(message))) {
                throw std::runtime_error("WM layout subscription queue overflow");
            }
            layout_sent_revision_ = snapshot->revision;
        }
        control_->Flush();
        if (control_->Closed()) {
            control_failed_ = true;
        }
    } catch (const std::exception &error) {
        PRISM_LOG_ERROR("WLR-LAYOUT", "%s", error.what());
        control_failed_ = true;
        control_->Close();
    }
    if (control_failed_) {
        CloseWindowControl();
        layout_controls_.Reset();
        CancelBoundaryPreview();
    }
}

} // namespace prism::wm
