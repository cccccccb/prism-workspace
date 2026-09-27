#include "prism/contracts/app_module.h"
#include <string_view>

namespace {
struct State { const PrismHostApiV1* host; double ticks{}; bool repeat{}; };
void Publish(State& state) {
    PrismValueV1 value{};
    value.kind=PRISM_VALUE_NUMBER_V1;
    value.as.number=state.ticks;
    state.host->set_binding(state.host->context,{"ticks",5},value);
}
void* Create(const PrismAppInitV1* init) {
    if (!init || !init->host) return nullptr;
    auto* state=new State{init->host};
    Publish(*state);
    if (init->host->backend_ready(init->host->context)) { delete state; return nullptr; }
    return state;
}
void Destroy(void* value) { delete static_cast<State*>(value); }
void Action(void* value,PrismStringViewV1 action) {
    auto& state=*static_cast<State*>(value);
    const std::string_view name(action.data,action.size);
    state.repeat=name=="repeat";
    if (name=="once" || state.repeat)
        state.host->schedule_tick(state.host->context,50000000);
}
void Tick(void* value,uint64_t) {
    auto& state=*static_cast<State*>(value);
    ++state.ticks;
    Publish(state);
    if (state.repeat) state.host->schedule_tick(state.host->context,50000000);
}
const PrismAppModuleV1 module{sizeof(module),PRISM_APP_ABI_V1,Create,Destroy,Action,Tick,nullptr,nullptr};
}
extern "C" PRISM_APP_EXPORT const PrismAppModuleV1* prism_app_module_v1() { return &module; }
