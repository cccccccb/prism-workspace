#include "prism/sdk/module_session.hpp"
#include <cstddef>
#include <vector>

namespace prism::sdk {
namespace {
PrismLogicalRectV1 Rect(const contracts::LogicalRect &rect)
{
    return {rect.x, rect.y, rect.width, rect.height};
}

PrismLogicalPointV1 Point(const contracts::LogicalPoint &point)
{
    return {point.x, point.y};
}

class LayoutViews {
public:
    explicit LayoutViews(const contracts::LayoutStateEvent &event)
    {
        const auto &snapshot = event.snapshot;
        outputs.reserve(snapshot.outputs.size());
        for (const auto &output : snapshot.outputs) {
            outputs.push_back({output.id, output.name.data(), output.name.size(),
                               Rect(output.logical_bounds), output.scale, output.primary,
                               output.supported});
        }
        workspaces.reserve(snapshot.workspaces.size());
        for (const auto &workspace : snapshot.workspaces) {
            workspaces.push_back({workspace.id, workspace.root, workspace.output,
                                  workspace.name.data(), workspace.name.size(), workspace.active,
                                  static_cast<std::uint32_t>(workspace.mode),
                                  workspace.mode_revision});
        }
        nodes.reserve(snapshot.nodes.size());
        for (const auto &node : snapshot.nodes) {
            nodes.push_back({node.id, node.parent, node.workspace,
                             static_cast<std::uint32_t>(node.kind),
                             static_cast<std::uint32_t>(node.layout), node.children.data(),
                             node.children.size(), Rect(node.tile_bounds), Rect(node.target_bounds),
                             Rect(node.committed_bounds), node.width_fraction, node.height_fraction,
                             node.instance.value, node.focused, node.visible, node.fullscreen,
                             node.has_committed});
        }
        boundaries.reserve(snapshot.boundaries.size());
        for (const auto &boundary : snapshot.boundaries) {
            boundaries.push_back({boundary.id, boundary.parent, boundary.first, boundary.second,
                                  boundary.workspace, static_cast<std::uint32_t>(boundary.axis),
                                  Rect(boundary.bounds), boundary.visible, boundary.resizable});
        }
        const auto &selected = snapshot.control_handle;
        handle = {sizeof(handle), selected.boundary, Rect(selected.bounds), selected.visible};

        state = {sizeof(state),
                 static_cast<std::uint32_t>(event.status),
                 event.subscription,
                 snapshot.session,
                 snapshot.revision,
                 snapshot.topology_revision,
                 snapshot.layout_revision,
                 snapshot.focus_revision,
                 snapshot.active_instance.value,
                 outputs.data(),
                 outputs.size(),
                 workspaces.data(),
                 workspaces.size(),
                 nodes.data(),
                 nodes.size(),
                 boundaries.data(),
                 boundaries.size(),
                 event.status == contracts::LayoutStateStatus::Current ? &handle : nullptr};
    }

    PrismLayoutStateV1 state{};

private:
    PrismLayoutControlHandleV1 handle{};
    std::vector<PrismLayoutOutputV1> outputs;
    std::vector<PrismLayoutWorkspaceV1> workspaces;
    std::vector<PrismLayoutNodeV1> nodes;
    std::vector<PrismLayoutBoundaryV1> boundaries;
};
} // namespace

uint64_t ModuleSession::SubscribeLayout(void *context, uint32_t enabled) noexcept
{
    if (!context || !static_cast<ModuleSession *>(context)->OnOwnerThread() || enabled > 1) {
        return 0;
    }
    try {
        auto &self = *static_cast<ModuleSession *>(context);
        if (self.closed_ || self.control_disconnected_ || !self.layout_subscribe_ ||
            !self.module_.Api().on_layout_state) {
            return 0;
        }
        if (enabled && self.layout_subscription_) {
            return self.layout_subscription_;
        }
        const auto request = self.layout_subscribe_(enabled != 0);
        if (request) {
            self.layout_subscription_ = enabled ? request : 0;
        }
        return request;
    } catch (...) {
        return 0;
    }
}

int32_t ModuleSession::ControlGesture(void *context, const PrismLayoutCommandV1 *command) noexcept
{
    if (!context || !command || !static_cast<ModuleSession *>(context)->OnOwnerThread() ||
        command->struct_size < sizeof(PrismLayoutCommandV1) ||
        command->phase > PRISM_GESTURE_CANCEL_V1) {
        return -1;
    }
    try {
        auto &self = *static_cast<ModuleSession *>(context);
        if (self.closed_ || self.control_disconnected_ || !self.module_.Api().on_gesture ||
            !self.module_.Api().on_layout_control_result) {
            return -1;
        }
        const auto gesture = command->gesture_id;
        const auto phase = static_cast<contracts::GesturePhase>(command->phase);
        bool accepted = false;
        if (phase == contracts::GesturePhase::Begin && !command->intent &&
            command->operation <= PRISM_LAYOUT_BOUNDARY_GESTURE_V1) {
            const auto &target = command->target;
            accepted = self.controls_.Begin(
                gesture, static_cast<contracts::LayoutControlOperation>(command->operation),
                {target.wm_session, target.output, target.workspace, target.root, target.boundary,
                 target.topology_revision, target.layout_revision});
        } else if (phase == contracts::GesturePhase::End &&
                   command->intent <= PRISM_LAYOUT_APPLY_BOUNDARY_V1) {
            accepted = self.controls_.EndIntent(
                gesture, static_cast<contracts::LayoutControlIntent>(command->intent));
        } else if (phase == contracts::GesturePhase::Cancel && !command->intent) {
            accepted = self.controls_.Cancel(gesture);
        }
        return accepted ? 0 : -1;
    } catch (...) {
        return -1;
    }
}

void ModuleSession::Gesture(const contracts::GestureEvent &event)
{
    if (!OnOwnerThread() || closed_ || !instance_) {
        return;
    }
    controls_.Observe(event);
    if (module_.Api().on_gesture) {
        PrismGestureEventV1 projected{sizeof(projected),
                                      static_cast<std::uint32_t>(event.phase),
                                      event.id,
                                      event.node.index,
                                      event.node.generation,
                                      event.action.data(),
                                      event.action.size(),
                                      event.source.seat,
                                      event.source.device,
                                      event.source.generation,
                                      event.touch,
                                      event.contact,
                                      event.serial,
                                      Point(event.start),
                                      Point(event.position),
                                      event.time_ns,
                                      event.snapshot_scene,
                                      event.snapshot_version};
        module_.Api().on_gesture(instance_, &projected);
    }
    controls_.Advance();
    DispatchControlResults();
}

void ModuleSession::Deliver(const contracts::LayoutStateEvent &event)
{
    if (!OnOwnerThread() || closed_ || !instance_ || !layout_subscription_ ||
        event.subscription != layout_subscription_ || !module_.Api().on_layout_state) {
        return;
    }
    if (event.status != contracts::LayoutStateStatus::Current) {
        layout_subscription_ = 0;
    }
    if (event.status == contracts::LayoutStateStatus::Disconnected) {
        control_disconnected_ = true;
        controls_.Disconnect();
        DispatchControlResults();
    }

    const LayoutViews views(event);
    module_.Api().on_layout_state(instance_, &views.state);
}

void ModuleSession::Deliver(const contracts::LayoutControlResult &event)
{
    if (!OnOwnerThread() || closed_) {
        return;
    }
    controls_.Receive(event);
    DispatchControlResults();
}

void ModuleSession::DispatchControlResults()
{
    const auto results = controls_.TakeResults();
    if (!instance_ || !module_.Api().on_layout_control_result) {
        return;
    }
    for (const auto &event : results) {
        PrismLayoutControlResultV1 projected{sizeof(projected),
                                             event.request,
                                             event.gesture,
                                             event.session,
                                             event.sequence,
                                             static_cast<std::uint32_t>(event.status),
                                             static_cast<std::uint32_t>(event.error),
                                             event.revision,
                                             event.topology_revision,
                                             event.layout_revision,
                                             Point(event.position),
                                             event.applied};
        module_.Api().on_layout_control_result(instance_, &projected);
    }
}

void ModuleSession::DisconnectControl()
{
    control_disconnected_ = true;
    controls_.Disconnect();
    DispatchControlResults();
    if (layout_subscription_) {
        Deliver(contracts::LayoutStateEvent{
            layout_subscription_, contracts::LayoutStateStatus::Disconnected, {}});
    }
}

} // namespace prism::sdk
