#include "prism/contracts/app_module.h"
#include <string.h>

static void *Create(const PrismAppInitV1 *init)
{
    const PrismValueV1 value = {.kind = PRISM_VALUE_BOOL_V1, .as.boolean = 0};
    init->host->set_binding(init->host->context, (PrismStringViewV1){"notifications", 13}, value);
    init->host->backend_ready(init->host->context);
    return (void *)init->host;
}

static void Destroy(void *instance)
{
    (void)instance;
}

static void ControlValue(void *instance, const PrismControlValueEventV1 *event)
{
    const PrismHostApiV1 *host = (const PrismHostApiV1 *)instance;
    if (event && event->struct_size >= sizeof(*event) && event->action.size == 4 &&
        memcmp(event->action.data, "echo", 4) == 0) {
        host->set_binding(host->context, (PrismStringViewV1){"before", 6}, event->before);
        host->set_binding(host->context, (PrismStringViewV1){"value", 5}, event->value);
        PrismValueV1 phase = {.kind = PRISM_VALUE_NUMBER_V1};
        phase.as.number = event->phase;
        host->set_binding(host->context, (PrismStringViewV1){"phase", 5}, phase);
        phase.as.number = event->cancel_reason;
        host->set_binding(host->context, (PrismStringViewV1){"reason", 6}, phase);
        return;
    }

    if (!event || event->struct_size < sizeof(*event) || event->phase != PRISM_CONTROL_COMMIT_V1 ||
        event->cancel_reason != PRISM_CONTROL_CANCEL_NONE_V1 || event->interaction == 0 ||
        event->node_generation == 0 || event->action.size != 6 ||
        memcmp(event->action.data, "notify", 6) != 0 || event->before.kind != PRISM_VALUE_BOOL_V1 ||
        event->value.kind != PRISM_VALUE_BOOL_V1) {
        return;
    }

    host->set_binding(host->context, (PrismStringViewV1){"notifications", 13}, event->value);
}

static const PrismAppModuleV1 api = {.struct_size = sizeof(api),
                                     .abi_version = PRISM_APP_ABI_V1,
                                     .create = Create,
                                     .destroy = Destroy,
                                     .on_control_value = ControlValue};

PRISM_APP_EXPORT const PrismAppModuleV1 *prism_app_module_v1(void)
{
    return &api;
}
