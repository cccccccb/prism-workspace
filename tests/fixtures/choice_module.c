#include "prism/contracts/app_module.h"
#include <string.h>

static int Is(PrismStringViewV1 text, const char *expected)
{
    return text.size == strlen(expected) && memcmp(text.data, expected, text.size) == 0;
}

static void *Create(const PrismAppInitV1 *init)
{
    const PrismValueV1 scheme = {.kind = PRISM_VALUE_STRING_V1, .as.string = {"light", 5}};
    const PrismValueV1 mode = {.kind = PRISM_VALUE_STRING_V1, .as.string = {"compact", 7}};
    init->host->set_binding(init->host->context, (PrismStringViewV1){"scheme", 6}, scheme);
    init->host->set_binding(init->host->context, (PrismStringViewV1){"mode", 4}, mode);
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
    if (!event || event->struct_size < sizeof(*event) || event->phase != PRISM_CONTROL_COMMIT_V1 ||
        event->before.kind != PRISM_VALUE_STRING_V1 || event->value.kind != PRISM_VALUE_STRING_V1) {
        return;
    }

    if ((Is(event->action, "scheme") &&
         (Is(event->value.as.string, "light") || Is(event->value.as.string, "dark"))) ||
        (Is(event->action, "mode") &&
         (Is(event->value.as.string, "compact") || Is(event->value.as.string, "comfortable")))) {
        host->set_binding(host->context, event->action, event->value);
    }
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
