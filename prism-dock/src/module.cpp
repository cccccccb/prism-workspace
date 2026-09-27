#include "prism/app/module_support.hpp"
#include "prism/contracts/launch.hpp"
#include <map>
#include <string>

namespace {
enum class Feedback { None, Pending, Complete, Failed };

struct RequestState {
    std::uint64_t id{};
    bool presented{}, ready{};
    Feedback feedback{Feedback::None};
};

struct Dock {
    const PrismHostApiV1 *host;
    std::map<std::uint64_t, std::string> windows;
    RequestState music, settings;

    bool Update()
    {
        bool music_running = false, settings_running = false;
        // The stable instance ID deduplicates snapshots/events and preserves
        // the group until the last mapped instance of that application stops.
        for (const auto &[id, app] : windows) {
            music_running = music_running || app == "demo_player";
            settings_running = settings_running || app == "demo_settings";
        }

        bool accepted = prism::app::Boolean(host, "music_visible", music_running);
        accepted = prism::app::Boolean(host, "settings_visible", settings_running) && accepted;
        accepted =
            prism::app::Boolean(host, "running_visible", music_running || settings_running) &&
            accepted;

        for (const auto &[prefix, state] :
             {std::pair{"music", &music}, std::pair{"settings", &settings}}) {
            const auto icon = state->feedback == Feedback::Failed     ? "error"
                              : state->feedback == Feedback::Complete ? "check"
                                                                      : "refresh";
            accepted =
                prism::app::Text(host, std::string(prefix) + "_feedback_icon", icon) && accepted;
            accepted = prism::app::Boolean(host, std::string(prefix) + "_feedback_visible",
                                           state->feedback != Feedback::None) &&
                       accepted;
        }

        return accepted;
    }
};

void *Create(const PrismAppInitV1 *init) noexcept
{
    if (!prism::app::ValidHost(init) ||
        init->host->struct_size < offsetof(PrismHostApiV1, subscribe_instances) +
                                      sizeof(init->host->subscribe_instances) ||
        !init->host->launch_app || !init->host->subscribe_instances) {
        return nullptr;
    }

    Dock *dock = nullptr;
    try {
        dock = new Dock{init->host, {}, {}, {}};
        if (dock->Update() && init->host->subscribe_instances(init->host->context) &&
            prism::app::Ready(init->host)) {
            return dock;
        }
    } catch (...) {
    }

    delete dock;
    return nullptr;
}

void Destroy(void *instance) noexcept
{
    delete static_cast<Dock *>(instance);
}

void Action(void *instance, PrismStringViewV1 value) noexcept
{
    try {
        auto &dock = *static_cast<Dock *>(instance);
        const std::string_view action(value.data, value.size);

        std::string_view app;
        RequestState *state{};
        if (action == "app:launch:music") {
            app = "demo_player";
            state = &dock.music;
        } else if (action == "app:launch:settings") {
            app = "demo_settings";
            state = &dock.settings;
        } else {
            return;
        }
        *state = {};
        state->id = dock.host->launch_app(dock.host->context, {app.data(), app.size()});
        state->feedback = state->id ? Feedback::Pending : Feedback::Failed;
        dock.Update();
    } catch (...) {
    }
}

void Instances(void *instance, const PrismInstanceEventV1 *event) noexcept
{
    try {
        if (!event || event->struct_size < sizeof(*event)) {
            return;
        }
        auto &dock = *static_cast<Dock *>(instance);
        if (event->change == 0) {
            dock.windows.clear();
        } else if (event->change == 1) {
            dock.windows[event->instance_id] = std::string(event->app_id.data, event->app_id.size);
        } else if (event->change == 2) {
            dock.windows.erase(event->instance_id);
        }
        dock.Update();
    } catch (...) {
    }
}

void LaunchEvent(void *instance, const PrismLaunchEventV1 *event) noexcept
{
    try {
        if (!event || event->struct_size < sizeof(*event) || !event->request_id) {
            return;
        }
        auto &dock = *static_cast<Dock *>(instance);
        const std::string_view app(event->app_id.data, event->app_id.size);
        auto *state = app == "demo_player"     ? &dock.music
                      : app == "demo_settings" ? &dock.settings
                                               : nullptr;
        if (!state || event->request_id != state->id) {
            return;
        }
        using prism::contracts::LaunchMilestone;
        const auto milestone = static_cast<LaunchMilestone>(event->milestone);
        if (event->error_code || milestone == LaunchMilestone::Failed) {
            state->feedback = Feedback::Failed;
        } else if (milestone == LaunchMilestone::Exited) {
            state->feedback = Feedback::None;
        } else {
            if (milestone == LaunchMilestone::FirstPresented) {
                state->presented = true;
            }
            if (milestone == LaunchMilestone::BackendReady) {
                state->ready = true;
            }
            if (milestone == LaunchMilestone::Activated || (state->presented && state->ready)) {
                state->feedback = Feedback::Complete;
            }
        }
        // Accepted only preserves Pending; it cannot create a running group or
        // a completion badge. Running groups come exclusively from Instances.
        dock.Update();
    } catch (...) {
    }
}

const PrismAppModuleV1 api{sizeof(api), PRISM_APP_ABI_V1, Create,    Destroy, Action,
                           nullptr,     LaunchEvent,      Instances, nullptr};
} // namespace

extern "C" PRISM_APP_EXPORT const PrismAppModuleV1 *prism_app_module_v1()
{
    return &api;
}
