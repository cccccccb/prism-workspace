#include "prism/app/module_support.hpp"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {
constexpr std::string_view groupGestureAction = "group:toggle-immersive";
constexpr double groupCommitDistance = 24;

bool SupportsLayout(const PrismHostApiV1 *host)
{
    return host->struct_size >=
               offsetof(PrismHostApiV1, control_gesture) + sizeof(host->control_gesture) &&
           host->subscribe_layout && host->control_gesture;
}

bool SameTarget(const PrismLayoutTargetV1 &first, const PrismLayoutTargetV1 &second)
{
    return first.wm_session == second.wm_session && first.output == second.output &&
           first.workspace == second.workspace && first.root == second.root &&
           first.topology_revision == second.topology_revision &&
           first.layout_revision == second.layout_revision;
}

struct Topbar {
    const PrismHostApiV1 *host;
    std::uint64_t subscription{};
    std::uint64_t revision{};
    PrismLayoutTargetV1 target{};
    std::uint32_t mode{PRISM_LAYOUT_GROUP_NORMAL_V1};
    std::uint64_t active_gesture{};
    PrismLayoutTargetV1 gesture_target{};
    std::uint32_t gesture_mode{PRISM_LAYOUT_GROUP_NORMAL_V1};
    bool gesture_ended{};

    bool Initialize()
    {
        if (!prism::app::Number(host, "group_immersive_opacity", 0) || !Update()) {
            return false;
        }

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

    void LayoutState(const PrismLayoutStateV1 &state)
    {
        if (state.struct_size < sizeof(state) || state.subscription_id != subscription) {
            return;
        }
        if (state.status != 0) {
            CancelGesture();
            target = {};
            revision = 0;
            return;
        }
        if (state.wm_session == target.wm_session && state.revision <= revision) {
            return;
        }

        const PrismLayoutWorkspaceV1 *active = nullptr;
        for (std::size_t i = 0; i < state.workspaces_size; ++i) {
            const auto &workspace = state.workspaces[i];
            if (!workspace.active) {
                continue;
            }
            if (active) {
                CancelGesture();
                target = {};
                return;
            }
            active = &workspace;
        }

        PrismLayoutTargetV1 next{};
        if (active && active->root &&
            (active->mode == PRISM_LAYOUT_GROUP_NORMAL_V1 ||
             active->mode == PRISM_LAYOUT_GROUP_IMMERSIVE_V1)) {
            for (std::size_t i = 0; i < state.outputs_size; ++i) {
                const auto &output = state.outputs[i];
                if (output.id == active->output && output.supported) {
                    next = {state.wm_session,     output.id, active->id,
                            active->root,         0,         state.topology_revision,
                            state.layout_revision};
                    break;
                }
            }
        }

        if (active_gesture && !gesture_ended &&
            (!SameTarget(gesture_target, next) || !active || active->mode != gesture_mode)) {
            CancelGesture();
        }
        target = next;
        revision = state.revision;
        if (target.wm_session) {
            mode = active->mode;
            prism::app::Number(host, "group_immersive_opacity",
                               mode == PRISM_LAYOUT_GROUP_IMMERSIVE_V1 ? 1 : 0);
        }
    }

    void Gesture(const PrismGestureEventV1 &event)
    {
        if (event.struct_size < sizeof(event) || event.touch || !event.action ||
            std::string_view(event.action, event.action_size) != groupGestureAction) {
            return;
        }

        PrismLayoutCommandV1 command{};
        command.struct_size = sizeof(command);
        command.gesture_id = event.gesture_id;
        command.phase = event.phase;
        command.operation = PRISM_LAYOUT_GROUP_GESTURE_V1;
        if (event.phase == PRISM_GESTURE_BEGIN_V1) {
            if (!target.wm_session || active_gesture) {
                return;
            }
            command.target = target;
            if (host->control_gesture(host->context, &command) == 0) {
                active_gesture = event.gesture_id;
                gesture_target = target;
                gesture_mode = mode;
                gesture_ended = false;
            }
            return;
        }
        if (event.gesture_id != active_gesture) {
            return;
        }

        if (event.phase == PRISM_GESTURE_END_V1) {
            // This selects an intent only. Host retains the real input proof;
            // the WM result and following snapshot determine whether it applied.
            const auto dx = event.position.x - event.start.x;
            const auto dy = event.position.y - event.start.y;
            if (std::isfinite(dx) && std::isfinite(dy) && dy >= groupCommitDistance &&
                dy >= std::abs(dx)) {
                command.intent = gesture_mode == PRISM_LAYOUT_GROUP_IMMERSIVE_V1
                                     ? PRISM_LAYOUT_EXIT_IMMERSIVE_V1
                                     : PRISM_LAYOUT_ENTER_IMMERSIVE_V1;
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

    bool Update()
    {
        std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        char clock[64];
        std::strftime(clock, sizeof(clock), "%b-%d %H:%M:%S", &local);
        std::string network_icon = "wifi-off";
        std::string power_icon = "power";

        std::error_code error;
        for (const auto &item : std::filesystem::directory_iterator("/sys/class/net", error)) {
            if (item.path().filename() == "lo") {
                continue;
            }
            std::string state;
            std::ifstream(item.path() / "operstate") >> state;
            if (state == "up") {
                network_icon = "wifi";
                break;
            }
        }

        for (const auto &item :
             std::filesystem::directory_iterator("/sys/class/power_supply", error)) {
            std::string type;
            std::ifstream(item.path() / "type") >> type;
            if (type == "Battery") {
                power_icon = "battery";
                break;
            }
        }

        return prism::app::Text(host, "clock_time", clock) &&
               prism::app::Text(host, "network_icon", network_icon) &&
               prism::app::Text(host, "power_icon", power_icon) && prism::app::Tick(host);
    }
};

void *Create(const PrismAppInitV1 *init) noexcept
{
    if (!prism::app::ValidHost(init)) {
        return nullptr;
    }

    Topbar *item = nullptr;
    try {
        item = new Topbar{init->host};
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
    delete static_cast<Topbar *>(instance);
}

void Tick(void *instance, std::uint64_t) noexcept
{
    try {
        static_cast<Topbar *>(instance)->Update();
    } catch (...) {
    }
}

void Gesture(void *instance, const PrismGestureEventV1 *event) noexcept
{
    if (!event) {
        return;
    }
    try {
        static_cast<Topbar *>(instance)->Gesture(*event);
    } catch (...) {
    }
}

void LayoutState(void *instance, const PrismLayoutStateV1 *state) noexcept
{
    if (!state) {
        return;
    }
    try {
        static_cast<Topbar *>(instance)->LayoutState(*state);
    } catch (...) {
    }
}

void ControlResult(void *instance, const PrismLayoutControlResultV1 *result) noexcept
{
    if (result) {
        static_cast<Topbar *>(instance)->ControlResult(*result);
    }
}

const PrismAppModuleV1 api{sizeof(api), PRISM_APP_ABI_V1, Create,       Destroy, nullptr,
                           Tick,        nullptr,          nullptr,      nullptr, nullptr,
                           Gesture,     LayoutState,      ControlResult};
} // namespace

extern "C" PRISM_APP_EXPORT const PrismAppModuleV1 *prism_app_module_v1()
{
    return &api;
}
