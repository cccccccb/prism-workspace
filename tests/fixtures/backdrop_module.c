#include "prism/contracts/app_module.h"
#include <stdlib.h>
#include <string.h>
struct State { const PrismHostApiV1* host; int phase; };
static int Color(struct State* state) {
    PrismValueV1 value={0}; value.kind=PRISM_VALUE_COLOR_V1;
    value.as.rgba=state->phase?0x005CFF40u:0xF02C3A40u;
    return state->host->set_binding(state->host->context,(PrismStringViewV1){"testTint",8},value);
}
static void* Create(const PrismAppInitV1* init) {
    struct State* state=calloc(1,sizeof(*state)); if(!state)return NULL;
    state->host=init->host;
    if(Color(state) || state->host->schedule_tick(state->host->context,700000000ULL) || state->host->backend_ready(state->host->context)){free(state);return NULL;}
    return state;
}
static void Destroy(void* state){free(state);}
static void Tick(void* value,uint64_t now){(void)now;struct State* state=value;state->phase=!state->phase;Color(state);state->host->schedule_tick(state->host->context,700000000ULL);}
static const PrismAppModuleV1 module={sizeof(module),PRISM_APP_ABI_V1,Create,Destroy,NULL,Tick,NULL,NULL};
PRISM_APP_EXPORT const PrismAppModuleV1* prism_app_module_v1(void){return &module;}
