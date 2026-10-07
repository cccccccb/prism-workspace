#include "owner_task_fixture.h"

#include <string.h>

/* A separate native fixture keeps module-owner-task unit tests immutable. Each
 * temporary package has a distinct DSO pathname and therefore its own state. */
struct State {
    const PrismHostApiV1 *host;
    size_t actions;
    size_t count;
    uint32_t reenter;
    uint64_t next_request;
    unsigned errors;
    OwnerTaskFixtureRecord records[24];
};

static struct State state;

static void *Create(const PrismAppInitV1 *init)
{
    if (!init || !init->host || init->host->struct_size < sizeof(PrismHostApiV1)) {
        return NULL;
    }

    memset(&state, 0, sizeof(state));
    state.host = init->host;
    /* This fixture has no background startup work. Declare business readiness
     * through the real ABI after initialization, like other native fixtures. */
    if (!state.host->backend_ready || state.host->backend_ready(state.host->context)) {
        return NULL;
    }
    return &state;
}

static void Destroy(void *instance)
{
    (void)instance;
}

static void Action(void *instance, PrismStringViewV1 action)
{
    struct State *current = instance;
    if (action.size == 12 && action.data && memcmp(action.data, "owner-action", 12) == 0) {
        ++current->actions;
    } else {
        ++current->errors;
    }
}

static void Completed(void *instance, const PrismTaskResultV1 *result)
{
    struct State *current = instance;
    if (!result || result->struct_size < sizeof(*result) || current->count >= 24 ||
        result->diagnostic.size > 1024 || (!result->diagnostic.data && result->diagnostic.size) ||
        result->file_path.size > 4096 || (!result->file_path.data && result->file_path.size) ||
        result->overwrite_approved > 1) {
        ++current->errors;
        return;
    }

    OwnerTaskFixtureRecord *record = &current->records[current->count++];
    record->request_id = result->request_id;
    record->outcome = result->outcome;
    record->choice_id = result->choice_id;
    record->cancel_reason = result->cancel_reason;
    record->failure_code = result->failure_code;
    record->diagnostic_size = result->diagnostic.size;
    if (result->diagnostic.size) {
        memcpy(record->diagnostic, result->diagnostic.data, result->diagnostic.size);
    }
    record->diagnostic[result->diagnostic.size] = '\0';
    record->file_path_size = result->file_path.size;
    if (result->file_path.size) {
        memcpy(record->file_path, result->file_path.data, result->file_path.size);
    }
    record->file_path[result->file_path.size] = '\0';
    record->overwrite_approved = result->overwrite_approved;

    if (current->reenter) {
        current->reenter = 0;
        const PrismTaskChoiceV1 choice = {
            sizeof(choice), 7, {"Continue", 8}, PRISM_TASK_CHOICE_PRIMARY_V1};
        const PrismTaskRequestV1 request = {sizeof(request),
                                            PRISM_TASK_CONFIRMATION_V1,
                                            {"Next request", 12},
                                            {"Continue this task", 18},
                                            &choice,
                                            1,
                                            NULL};
        current->next_request = current->host->request_task(current->host->context, &request);
        if (!current->next_request) {
            ++current->errors;
        }
    }
}

static const PrismAppModuleV1 api = {.struct_size = sizeof(api),
                                     .abi_version = PRISM_APP_ABI_V1,
                                     .create = Create,
                                     .destroy = Destroy,
                                     .on_action = Action,
                                     .on_task_completed = Completed};

PRISM_APP_EXPORT const PrismAppModuleV1 *prism_app_module_v1(void)
{
    return &api;
}

const PrismHostApiV1 *owner_task_provider_host(void)
{
    return state.host;
}

size_t owner_task_provider_actions(void)
{
    return state.actions;
}

size_t owner_task_provider_count(void)
{
    return state.count;
}

const OwnerTaskFixtureRecord *owner_task_provider_record(size_t index)
{
    return index < state.count ? &state.records[index] : NULL;
}

void owner_task_provider_reenter(uint32_t enabled)
{
    state.reenter = enabled;
}

uint64_t owner_task_provider_next_request(void)
{
    return state.next_request;
}

unsigned owner_task_provider_errors(void)
{
    return state.errors;
}
