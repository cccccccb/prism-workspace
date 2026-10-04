#include "prism/app/module_support.hpp"
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {
constexpr std::string_view boundaryGestureAction = "boundary:resize";

bool SupportsLayout(const PrismHostApiV1 *host)
{
    return host->struct_size >=
               offsetof(PrismHostApiV1, control_gesture) + sizeof(host->control_gesture) &&
           host->subscribe_layout && host->control_gesture;
}

bool SameIdentity(const PrismLayoutTargetV1 &first, const PrismLayoutTargetV1 &second)
{
    // Live resizing advances layout_revision. The WM owns session invalidation
    // for external geometry changes, while the module retains the Begin target.
    return first.wm_session == second.wm_session && first.output == second.output &&
           first.workspace == second.workspace && first.root == second.root &&
           first.boundary == second.boundary && first.topology_revision == second.topology_revision;
}

const PrismLayoutBoundaryV1 *SelectedBoundary(const PrismLayoutStateV1 &state)
{
    if (state.struct_size <
            offsetof(PrismLayoutStateV1, control_handle) + sizeof(state.control_handle) ||
        !state.control_handle ||
        state.control_handle->struct_size < offsetof(PrismLayoutControlHandleV1, node) ||
        !state.control_handle->visible || !state.control_handle->boundary) {
        return nullptr;
    }

    for (std::size_t i = 0; i < state.boundaries_size; ++i) {
        const auto &boundary = state.boundaries[i];
        if (boundary.id == state.control_handle->boundary && boundary.visible &&
            boundary.resizable && boundary.axis <= 1) {
            return &boundary;
        }
    }
    return nullptr;
}

PrismLayoutTargetV1 Target(const PrismLayoutStateV1 &state, std::uint64_t workspace_id,
                          std::uint64_t boundary_id)
{
    if (!workspace_id) {
        return {};
    }

    for (std::size_t i = 0; i < state.workspaces_size; ++i) {
        const auto &workspace = state.workspaces[i];
        if (workspace.id != workspace_id || !workspace.active || !workspace.root) {
            continue;
        }

        for (std::size_t j = 0; j < state.outputs_size; ++j) {
            const auto &output = state.outputs[j];
            if (output.id == workspace.output && output.supported) {
                return {state.wm_session,     output.id,    workspace.id,
                        workspace.root,       boundary_id, state.topology_revision,
                        state.layout_revision};
            }
        }
    }
    return {};
}

struct LayoutControls {
    const PrismHostApiV1 *host;
    std::uint64_t subscription{};
    std::uint64_t session{};
    std::uint64_t revision{};
    PrismLayoutTargetV1 target{};
    std::uint64_t active_gesture{};
    PrismLayoutTargetV1 gesture_target{};
    bool gesture_ended{};
    std::uint64_t window_node{}, gesture_node{};
    PrismLayoutIntentV1 window_intent{PRISM_LAYOUT_INTENT_NONE_V1};
    PrismLayoutIntentV1 gesture_intent{PRISM_LAYOUT_INTENT_NONE_V1};
    double button_x{};

    bool Initialize()
    {
        if (!prism::app::Boolean(host, "handle_visible", false) ||
            !prism::app::Number(host, "handle_width", 4) ||
            !prism::app::Number(host, "handle_height", 32)) {
            return false;
        }

        prism::app::Boolean(host, "window_visible", false);
        prism::app::Boolean(host, "split_enabled", false);
        prism::app::Number(host, "split_opacity", 0.35);
        prism::app::Text(host, "fullscreen_icon", "fullscreen");
        if (SupportsLayout(host)) {
            subscription = host->subscribe_layout(host->context, 1);
        }
        return prism::app::Ready(host);
    }

    void ClearGesture()
    {
        active_gesture = 0;
        gesture_target = {};
        gesture_ended = false;
        gesture_node = 0;
        gesture_intent = PRISM_LAYOUT_INTENT_NONE_V1;
    }

    void CancelGesture()
    {
        if (active_gesture && !gesture_ended) {
            PrismLayoutCommandV1 command{};
            command.struct_size = sizeof(command);
            command.gesture_id = active_gesture;
            command.phase = PRISM_GESTURE_CANCEL_V1;
            host->control_gesture(host->context, &command);
        }
        ClearGesture();
    }

    void Hide()
    {
        CancelGesture();
        target = {};
        window_node = 0;
        prism::app::Boolean(host, "window_visible", false);
        prism::app::Boolean(host, "handle_visible", false);
    }

    void LayoutState(const PrismLayoutStateV1 &state)
    {
        constexpr auto required =
            offsetof(PrismLayoutStateV1, boundaries_size) + sizeof(state.boundaries_size);
        if (state.struct_size < required || !subscription ||
            state.subscription_id != subscription) {
            return;
        }
        if (state.status != 0) {
            Hide();
            session = revision = 0;
            return;
        }
        if (state.wm_session == session && state.revision <= revision) {
            return;
        }

        const auto *boundary = SelectedBoundary(state);
        const PrismLayoutNodeV1 *selected{};
        if (state.struct_size >= offsetof(PrismLayoutStateV1, control_handle) + sizeof(state.control_handle) &&
            state.control_handle && state.control_handle->struct_size >= sizeof(*state.control_handle) &&
            state.control_handle->visible && state.control_handle->node) {
            for (std::size_t i = 0; i < state.nodes_size; ++i) {
                if (state.nodes[i].id == state.control_handle->node && state.nodes[i].visible && state.nodes[i].kind == 1) {
                    selected = &state.nodes[i];
                    break;
                }
            }
        }
        const auto next = Target(state, selected ? selected->workspace : boundary ? boundary->workspace : 0,
                                 boundary ? boundary->id : 0);
        const auto next_node = selected ? selected->id : 0;
        if (active_gesture && !gesture_ended &&
            (!SameIdentity(gesture_target, next) || gesture_node != next_node)) {
            CancelGesture();
        }
        window_node = next_node;
        window_intent = selected && selected->fullscreen ? PRISM_LAYOUT_EXIT_WINDOW_FULLSCREEN_V1
                                                          : PRISM_LAYOUT_ENTER_WINDOW_FULLSCREEN_V1;
        bool split = false;
        if (selected && !selected->fullscreen) {
            for (std::size_t i = 0; i < state.nodes_size; ++i) {
                split |= state.nodes[i].id == selected->parent && state.nodes[i].children_size > 1;
            }
        }
        prism::app::Boolean(host, "window_visible", window_node != 0);
        prism::app::Boolean(host, "split_enabled", split);
        prism::app::Number(host, "split_opacity", split ? 1.0 : 0.35);
        prism::app::Text(host, "fullscreen_icon", selected && selected->fullscreen ? "restore" : "fullscreen");
        target = next;
        session = state.wm_session;
        revision = state.revision;
        if (target.wm_session && boundary) {
            prism::app::Number(host, "handle_width", boundary->axis == 0 ? 4 : 32);
            prism::app::Number(host, "handle_height", boundary->axis == 0 ? 32 : 4);
        }
        prism::app::Boolean(host, "handle_visible", target.wm_session != 0 && !window_node);
    }

    void Gesture(const PrismGestureEventV1 &event)
    {
        if (event.struct_size < sizeof(event) || event.touch || !event.action) {
            return;
        }

        const std::string_view action(event.action, event.action_size);
        const bool window = action == "window:fullscreen" || action == "window:horizontal" || action == "window:vertical";
        if (!window && action != boundaryGestureAction) {
            return;
        }
        PrismLayoutCommandV1 command{};
        command.struct_size = sizeof(command);
        command.gesture_id = event.gesture_id;
        command.phase = event.phase;
        command.operation = window ? PRISM_LAYOUT_WINDOW_GESTURE_V1 : PRISM_LAYOUT_BOUNDARY_GESTURE_V1;
        if (event.phase == PRISM_GESTURE_BEGIN_V1) {
            if (!target.wm_session || active_gesture || window != bool(window_node)) {
                return;
            }
            command.target = target;
            command.node = window_node;
            if (host->control_gesture(host->context, &command) == 0) {
                active_gesture = event.gesture_id;
                gesture_target = target;
                gesture_node = window_node;
                gesture_intent = action == "window:horizontal" ? PRISM_LAYOUT_SPLIT_HORIZONTAL_V1
                                   : action == "window:vertical" ? PRISM_LAYOUT_SPLIT_VERTICAL_V1 : window_intent;
                button_x = action == "window:horizontal" ? 60 : action == "window:vertical" ? 112 : 8;
                gesture_ended = false;
            }
            return;
        }
        if (event.gesture_id != active_gesture) {
            return;
        }

        if (event.phase == PRISM_GESTURE_END_V1) {
            if (window && (event.position.x < button_x || event.position.x >= button_x + 48 ||
                           event.position.y < 8 || event.position.y >= 48)) {
                command.phase = PRISM_GESTURE_CANCEL_V1;
            } else {
                command.intent = window ? gesture_intent : PRISM_LAYOUT_APPLY_BOUNDARY_V1;
            }
            gesture_ended = true;
            if (host->control_gesture(host->context, &command) != 0) {
                ClearGesture();
            }
        } else if (event.phase == PRISM_GESTURE_CANCEL_V1) {
            ClearGesture();
        }
    }

    void ControlResult(const PrismLayoutControlResultV1 &result)
    {
        if (result.struct_size < sizeof(result) || result.gesture_id != active_gesture) {
            return;
        }
        if (result.status == PRISM_LAYOUT_ENDED_V1 || result.status == PRISM_LAYOUT_CANCELLED_V1 ||
            result.status == PRISM_LAYOUT_REJECTED_V1) {
            ClearGesture();
        }
    }
};

void *Create(const PrismAppInitV1 *init) noexcept
{
    if (!prism::app::ValidHost(init)) {
        return nullptr;
    }

    LayoutControls *item = nullptr;
    try {
        item = new LayoutControls{init->host};
        if (item->Initialize()) {
            return item;
        }
    } catch (...) {
    }

    delete item;
    return nullptr;
}

void Destroy(void *instance) noexcept
{
    delete static_cast<LayoutControls *>(instance);
}

void Gesture(void *instance, const PrismGestureEventV1 *event) noexcept
{
    if (!event) {
        return;
    }
    try {
        static_cast<LayoutControls *>(instance)->Gesture(*event);
    } catch (...) {
    }
}

void LayoutState(void *instance, const PrismLayoutStateV1 *state) noexcept
{
    if (!state) {
        return;
    }
    try {
        static_cast<LayoutControls *>(instance)->LayoutState(*state);
    } catch (...) {
    }
}

void ControlResult(void *instance, const PrismLayoutControlResultV1 *result) noexcept
{
    if (result) {
        static_cast<LayoutControls *>(instance)->ControlResult(*result);
    }
}

const PrismAppModuleV1 api{sizeof(api), PRISM_APP_ABI_V1, Create,       Destroy, nullptr,
                           nullptr,     nullptr,          nullptr,      nullptr, nullptr,
                           Gesture,     LayoutState,      ControlResult};
} // namespace

extern "C" PRISM_APP_EXPORT const PrismAppModuleV1 *prism_app_module_v1()
{
    return &api;
}
