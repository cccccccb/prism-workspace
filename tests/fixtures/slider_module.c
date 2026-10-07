#include "prism/contracts/app_module.h"
#include <stdio.h>
#include <string.h>

static void Label(const PrismHostApiV1 *host, double number)
{
    char buffer[24];
    const int size = snprintf(buffer, sizeof(buffer), "%.0f%%", number);
    const PrismValueV1 value = {.kind = PRISM_VALUE_STRING_V1,
                                .as.string = {buffer, (uint32_t)size}};
    host->set_binding(host->context, (PrismStringViewV1){"volumeLabel", 11}, value);
}

static void *Create(const PrismAppInitV1 *init)
{
    const PrismValueV1 value = {.kind = PRISM_VALUE_NUMBER_V1, .as.number = 62};
    init->host->set_binding(init->host->context, (PrismStringViewV1){"volume", 6}, value);
    Label(init->host, 62);
    init->host->backend_ready(init->host->context);
    return (void *)init->host;
}

static void Destroy(void *instance)
{
    (void)instance;
}

static void Control(void *instance, const PrismControlValueEventV1 *event)
{
    if (!event || event->struct_size < sizeof(*event) || event->action.size != 6 ||
        memcmp(event->action.data, "volume", 6) || event->value.kind != PRISM_VALUE_NUMBER_V1) {
        return;
    }
    const PrismHostApiV1 *host = instance;
    Label(host, event->value.as.number);
    if (event->phase == PRISM_CONTROL_COMMIT_V1) {
        host->set_binding(host->context, (PrismStringViewV1){"volume", 6}, event->value);
    }
}

static const PrismAppModuleV1 api = {.struct_size = sizeof(api),
                                     .abi_version = PRISM_APP_ABI_V1,
                                     .create = Create,
                                     .destroy = Destroy,
                                     .on_control_value = Control};

PRISM_APP_EXPORT const PrismAppModuleV1 *prism_app_module_v1(void)
{
    return &api;
}
