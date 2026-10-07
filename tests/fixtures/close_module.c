#include "close_fixture.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

_Static_assert(offsetof(PrismHostApiV1, complete_close) ==
                   offsetof(PrismHostApiV1, cancel_task) +
                       sizeof(((PrismHostApiV1 *)0)->cancel_task),
               "Close continuation appends to the Host prefix");
_Static_assert(offsetof(PrismAppModuleV1, on_close_request) ==
                   offsetof(PrismAppModuleV1, on_task_completed) +
                       sizeof(((PrismAppModuleV1 *)0)->on_task_completed),
               "Close continuation appends to the module prefix");

static const PrismHostApiV1 *host;
static uint64_t request_id;
static unsigned calls;
static int32_t decision = PRISM_CLOSE_DEFER_V1;
static int32_t synchronous_decision = -1;
static int32_t create_completion;
static int32_t synchronous_completion;
static int32_t duplicate_completion;
static int32_t destroy_completion;
#ifdef TEST_CLOSE_PREFIX
static void *mapping;
static size_t page;
#endif

static void *Create(const PrismAppInitV1 *init)
{
    if (!init || !init->host ||
        init->host->struct_size <
            offsetof(PrismHostApiV1, complete_close) + sizeof(init->host->complete_close) ||
        !init->host->complete_close) {
        return NULL;
    }
    host = init->host;
    request_id = 0;
    calls = 0;
    decision = PRISM_CLOSE_DEFER_V1;
    synchronous_decision = -1;
    synchronous_completion = duplicate_completion = destroy_completion = 99;
    create_completion = host->complete_close(host->context, 1, PRISM_CLOSE_ACCEPT_V1);
    return (void *)host;
}

static void Destroy(void *instance)
{
    (void)instance;
    destroy_completion = host->complete_close(host->context, request_id, PRISM_CLOSE_ACCEPT_V1);
}

static int32_t LegacyClose(void *instance)
{
    (void)instance;
    ++calls;
    return decision;
}

static int32_t Close(void *instance, const PrismCloseRequestV1 *request)
{
    (void)instance;
    if (!request || request->struct_size < sizeof(*request) || !request->request_id) {
        return 77;
    }
    request_id = request->request_id;
    ++calls;
    if (synchronous_decision >= 0) {
        synchronous_completion =
            host->complete_close(host->context, request_id, (uint32_t)synchronous_decision);
        duplicate_completion =
            host->complete_close(host->context, request_id, PRISM_CLOSE_ACCEPT_V1);
    }
    const PrismValueV1 value = {.kind = PRISM_VALUE_NUMBER_V1, .as.number = (double)request_id};
    host->set_binding(host->context, (PrismStringViewV1){"insideclose", 11}, value);
    return decision;
}

static const PrismAppModuleV1 module = {.struct_size = sizeof(module),
                                        .abi_version = PRISM_APP_ABI_V1,
                                        .create = Create,
                                        .destroy = Destroy,
                                        .on_close_requested = LegacyClose,
                                        .on_close_request = Close};

PRISM_APP_EXPORT const PrismAppModuleV1 *prism_app_module_v1(void)
{
#ifdef TEST_CLOSE_PREFIX
    if (!mapping) {
        page = (size_t)sysconf(_SC_PAGESIZE);
        mapping = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED || mprotect((char *)mapping + page, page, PROT_NONE)) {
            abort();
        }
        const size_t prefix = offsetof(PrismAppModuleV1, on_close_request);
        memcpy((char *)mapping + page - prefix, &module, prefix);
        PrismAppModuleV1 *old = (PrismAppModuleV1 *)((char *)mapping + page - prefix);
        old->struct_size = (uint32_t)prefix;
#ifdef TEST_CLOSE_PARTIAL
        old->struct_size += sizeof(module.on_close_request) - 1;
#endif
    }
    return (const PrismAppModuleV1 *)((char *)mapping + page -
                                      offsetof(PrismAppModuleV1, on_close_request));
#else
    return &module;
#endif
}

const PrismHostApiV1 *close_fixture_host(void)
{
    return host;
}

void close_fixture_mode(int32_t next_decision, int32_t next_synchronous)
{
    decision = next_decision;
    synchronous_decision = next_synchronous;
}

uint64_t close_fixture_request(void)
{
    return request_id;
}

unsigned close_fixture_calls(void)
{
    return calls;
}

int32_t close_fixture_create_completion(void)
{
    return create_completion;
}

int32_t close_fixture_synchronous_completion(void)
{
    return synchronous_completion;
}

int32_t close_fixture_duplicate_completion(void)
{
    return duplicate_completion;
}

int32_t close_fixture_destroy_completion(void)
{
    return destroy_completion;
}

#ifdef TEST_CLOSE_PREFIX
__attribute__((destructor)) static void Unload(void)
{
    if (mapping && mapping != MAP_FAILED) {
        munmap(mapping, page * 2);
    }
}
#endif
