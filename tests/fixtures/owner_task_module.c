#include "owner_task_fixture.h"

#include <stddef.h>
#include <string.h>

_Static_assert(offsetof(PrismHostApiV1, task_capabilities) ==
                   offsetof(PrismHostApiV1, control_gesture) +
                       sizeof(((PrismHostApiV1 *)0)->control_gesture),
               "Owner tasks append to the existing Host prefix");
_Static_assert(offsetof(PrismHostApiV1, request_task) ==
                   offsetof(PrismHostApiV1, task_capabilities) +
                       sizeof(((PrismHostApiV1 *)0)->task_capabilities),
               "Request follows capability query");
_Static_assert(offsetof(PrismHostApiV1, cancel_task) ==
                   offsetof(PrismHostApiV1, request_task) +
                       sizeof(((PrismHostApiV1 *)0)->request_task),
               "Cancel follows request submission");
_Static_assert(offsetof(PrismAppModuleV1, on_task_completed) ==
                   offsetof(PrismAppModuleV1, on_control_value) +
                       sizeof(((PrismAppModuleV1 *)0)->on_control_value),
               "Task completion appends to the existing module prefix");
_Static_assert(offsetof(PrismTaskRequestV1, file) ==
                   offsetof(PrismTaskRequestV1, choices_size) +
                       sizeof(((PrismTaskRequestV1 *)0)->choices_size),
               "File options append to the original request prefix");
_Static_assert(offsetof(PrismTaskResultV1, file_path) ==
                   offsetof(PrismTaskResultV1, diagnostic) +
                       sizeof(((PrismTaskResultV1 *)0)->diagnostic),
               "File result appends to the original completion prefix");

struct State {
    const PrismHostApiV1 *host;
    uint64_t create_request;
    uint64_t next_request;
    size_t count;
    uint32_t reenter;
    unsigned errors;
    OwnerTaskFixtureRecord records[32];
};

static struct State state;

static uint64_t Request(const PrismHostApiV1 *host)
{
    const PrismTaskChoiceV1 choice = {
        sizeof(choice), 7, {"Continue", 8}, PRISM_TASK_CHOICE_PRIMARY_V1};
    const PrismTaskRequestV1 request = {sizeof(request),
                                        PRISM_TASK_CONFIRMATION_V1,
                                        {"Next request", 12},
                                        {"Continue this task", 18},
                                        &choice,
                                        1,
                                        NULL};
    return host->request_task(host->context, &request);
}

static void *Create(const PrismAppInitV1 *init)
{
    if (!init || !init->host ||
        init->host->struct_size <
            offsetof(PrismHostApiV1, cancel_task) + sizeof(init->host->cancel_task) ||
        !init->host->request_task || !init->host->cancel_task || !init->host->task_capabilities) {
        return NULL;
    }

    memset(&state, 0, sizeof(state));
    state.host = init->host;
    state.create_request = Request(state.host);
    return &state;
}

static void Destroy(void *instance)
{
    (void)instance;
}

static void Completed(void *instance, const PrismTaskResultV1 *result)
{
    struct State *current = (struct State *)instance;
    if (!result || result->struct_size < sizeof(*result) || current->count >= 32 ||
        result->diagnostic.size > 1024 || (!result->diagnostic.data && result->diagnostic.size) ||
        result->file_path.size > 4096 || (!result->file_path.data && result->file_path.size) ||
        result->overwrite_approved > 1) {
        ++current->errors;
        return;
    }

    /* This synchronous Host callback can mutate the C++ caller's source result.
     * The following copy must still see the immutable projected terminal. */
    const PrismValueV1 value = {.kind = PRISM_VALUE_NUMBER_V1,
                                .as.number = (double)(current->count + 1)};
    if (current->host->set_binding(current->host->context, (PrismStringViewV1){"completed", 9},
                                   value)) {
        ++current->errors;
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
        current->next_request = Request(current->host);
    }
}

static const PrismAppModuleV1 api = {.struct_size = sizeof(api),
                                     .abi_version = PRISM_APP_ABI_V1,
                                     .create = Create,
                                     .destroy = Destroy,
                                     .on_task_completed = Completed};

PRISM_APP_EXPORT const PrismAppModuleV1 *prism_app_module_v1(void)
{
    return &api;
}

const PrismHostApiV1 *owner_task_fixture_host(void)
{
    return state.host;
}

uint64_t owner_task_fixture_create_request(void)
{
    return state.create_request;
}

size_t owner_task_fixture_count(void)
{
    return state.count;
}

const OwnerTaskFixtureRecord *owner_task_fixture_record(size_t index)
{
    return index < state.count ? &state.records[index] : NULL;
}

void owner_task_fixture_reenter(uint32_t enabled)
{
    state.reenter = enabled;
}

uint64_t owner_task_fixture_next_request(void)
{
    return state.next_request;
}

unsigned owner_task_fixture_errors(void)
{
    return state.errors;
}
