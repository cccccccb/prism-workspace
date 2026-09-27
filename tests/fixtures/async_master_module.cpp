#include "prism/contracts/app_module.h"
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {
struct State {
    const PrismHostApiV1 *host;
};

void Require(bool condition, const char *detail)
{
    if (!condition) {
        std::fprintf(stderr, "async_master_fixture: %s\n", detail);
        std::abort();
    }
}

void SetStatus(State &state, std::string_view text)
{
    PrismValueV1 value{};
    value.kind = PRISM_VALUE_STRING_V1;
    value.as.string = {text.data(), text.size()};
    Require(state.host->set_binding(state.host->context, {"status", 6}, value) == 0,
            "Host rejected declared status binding");
}

void *Create(const PrismAppInitV1 *init)
{
    if (!init || !init->host) {
        return nullptr;
    }
    auto *state = new State{init->host};
    SetStatus(*state, "Master business ready");
    Require(state->host->launch_app(state->host->context, {"async-probe-other", 17}) == 9,
            "Host did not register deterministic launch request");
    Require(state->host->subscribe_instances(state->host->context) == 9,
            "Host did not register deterministic instance subscription");
    if (state->host->backend_ready(state->host->context)) {
        delete state;
        return nullptr;
    }
    return state;
}

void Destroy(void *value)
{
    delete static_cast<State *>(value);
}

void Action(void *value, PrismStringViewV1)
{
    SetStatus(*static_cast<State *>(value), "Master action received");
}

void Launch(void *value, const PrismLaunchEventV1 *event)
{
    Require(event && event->request_id == 9 &&
                std::string_view(event->app_id.data, event->app_id.size) == "async-probe-other",
            "Host projected an incorrect launch request");
    SetStatus(*static_cast<State *>(value), "Master launch event received");
}

void Instance(void *value, const PrismInstanceEventV1 *event)
{
    Require(event && event->subscription_id == 9, "Host projected an incorrect subscription");
    SetStatus(*static_cast<State *>(value), "Master instance event received");
}

void Theme(void *value, const PrismThemeEventV1 *event)
{
    Require(event && event->request_id == 0, "Fixture expected a broadcast theme event");
    SetStatus(*static_cast<State *>(value), "Master theme event received");
}

const PrismAppModuleV1 module{sizeof(module), PRISM_APP_ABI_V1, Create,   Destroy, Action,
                              nullptr,        Launch,           Instance, Theme};
} // namespace

extern "C" PRISM_APP_EXPORT const PrismAppModuleV1 *prism_app_module_v1()
{
    return &module;
}
