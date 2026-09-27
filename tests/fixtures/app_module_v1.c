#include "prism/contracts/app_module.h"
#include <stdlib.h>
#include <string.h>

#ifndef TEST_MODULE_ABI
#define TEST_MODULE_ABI PRISM_APP_ABI_V1
#endif
struct Backend {
    const PrismHostApiV1 *host;
};

static void *create(const PrismAppInitV1 *init)
{
    if (!init || init->struct_size < sizeof(*init) || init->abi_version != PRISM_APP_ABI_V1 ||
        !init->instance_id || !init->host || init->host->struct_size < sizeof(*init->host) ||
        init->host->abi_version != PRISM_APP_ABI_V1) {
        return NULL;
    }
    struct Backend *instance = malloc(sizeof(*instance));
    if (!instance) {
        return NULL;
    }
    instance->host = init->host;
    return instance;
}
#ifndef TEST_MISSING_DESTROY
static void destroy(void *value)
{
    free(value);
}
#endif
static void action(void *value, PrismStringViewV1 event)
{
    struct Backend *backend = value;
    if (event.size == 4 && memcmp(event.data, "play", 4) == 0) {
        PrismValueV1 state = {0};
        state.kind = PRISM_VALUE_BOOL_V1;
        state.as.boolean = 1;
        PrismStringViewV1 key = {"playing", 7};
        backend->host->set_binding(backend->host->context, key, state);
        backend->host->backend_ready(backend->host->context);
    }
}

static const PrismAppModuleV1 module = {sizeof(PrismAppModuleV1),
                                        TEST_MODULE_ABI,
                                        create,
#ifdef TEST_MISSING_DESTROY
                                        NULL,
#else
                                        destroy,
#endif
                                        action,
                                        NULL,
                                        NULL,
                                        NULL};
const PrismAppModuleV1 *prism_app_module_v1(void)
{
    return &module;
}
