#include "prism/contracts/app_module.h"
/* Exact pre-subscription v1 layout: exercise loader without reading a new tail. */
struct Legacy {
    uint32_t struct_size, abi_version;
    void* (*create)(const PrismAppInitV1*);
    void (*destroy)(void*);
    void (*on_action)(void*,PrismStringViewV1);
    void (*on_tick)(void*,uint64_t);
    void (*on_launch_event)(void*,const PrismLaunchEventV1*);
};
static void* create(const PrismAppInitV1* init) { return (void*)init->host; }
static void destroy(void* instance) { (void)instance; }
static const struct Legacy api={sizeof(api),PRISM_APP_ABI_V1,create,destroy,0,0,0};
const PrismAppModuleV1* prism_app_module_v1(void) { return (const PrismAppModuleV1*)&api; }
