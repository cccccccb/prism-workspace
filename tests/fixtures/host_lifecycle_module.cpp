#include "prism/contracts/app_module.h"
#include <csignal>
#include <chrono>
#include <cstdio>
#include <string_view>
#include <thread>
namespace {
struct State { const PrismHostApiV1* host; };
void* Create(const PrismAppInitV1* init) {
#ifdef TEST_HANG_INIT
    std::this_thread::sleep_for(std::chrono::seconds(30));
#endif
#ifdef TEST_CRASH_INIT
    raise(SIGSEGV);
#endif
    if (!init || !init->host) return nullptr;
    PrismValueV1 status{}; status.kind = PRISM_VALUE_STRING_V1; status.as.string = {"Parent ready", 12};
    if (init->host->set_binding(init->host->context, {"status", 6}, status)) return nullptr;
#ifdef TEST_CHILD_LAUNCH
    if (!init->host->launch_app(init->host->context, {"demo_player", 11})) return nullptr;
#endif
    if (init->host->backend_ready(init->host->context)) return nullptr;
    return new State{init->host};
}
void Destroy(void* value) { delete static_cast<State*>(value); }
void Event(void* value, const PrismLaunchEventV1* event) {
    std::printf("fixture child milestone=%u instance=%llu pid=%u app=%.*s\n", event->milestone,
        static_cast<unsigned long long>(event->instance_id), event->pid,
        static_cast<int>(event->app_id.size), event->app_id.data);
    std::fflush(stdout);
    if (event->milestone == 5) {
        PrismValueV1 status{}; status.kind = PRISM_VALUE_STRING_V1; status.as.string = {"Child ready", 11};
        auto& state = *static_cast<State*>(value);
        state.host->set_binding(state.host->context, {"status", 6}, status);
    }
}
const PrismAppModuleV1 module{sizeof(module), PRISM_APP_ABI_V1, Create, Destroy, nullptr, nullptr, Event, nullptr};
}
extern "C" PRISM_APP_EXPORT const PrismAppModuleV1* prism_app_module_v1() { return &module; }
