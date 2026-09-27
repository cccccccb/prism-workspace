#include "prism/contracts/app_module.h"
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>

__attribute__((constructor)) static void Load(void)
{
    const char *entered = getenv("PRISM_WORK_TEST_LOAD_ENTER");
    const char *gate = getenv("PRISM_WORK_TEST_LOAD_GATE");
    if (entered && gate) {
        const uint64_t value = 1;
        if (write(atoi(entered), &value, sizeof(value)) != sizeof(value)) {
            abort();
        }
        struct pollfd source = {atoi(gate), POLLIN, 0};
        if (poll(&source, 1, 5000) != 1 || !(source.revents & POLLIN)) {
            abort();
        }
    }
}

static void *Create(const PrismAppInitV1 *init)
{
    init->host->backend_ready(init->host->context);
    return (void *)init->host;
}

static void Destroy(void *instance)
{
    (void)instance;
}

PRISM_APP_EXPORT const PrismAppModuleV1 *prism_app_module_v1(void)
{
    static const PrismAppModuleV1 api = {
        sizeof(api), PRISM_APP_ABI_V1, Create, Destroy, 0, 0, 0, 0, 0, 0};
    return &api;
}
