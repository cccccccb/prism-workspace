#include "prism/contracts/app_module.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* The exact module layout before owner tasks. The adjacent new callback pointer
 * is protected even when the advertised struct_size includes part of it. */
struct Prefix {
    uint32_t struct_size, abi_version;
    void *(*create)(const PrismAppInitV1 *);
    void (*destroy)(void *);
    void (*on_action)(void *, PrismStringViewV1);
    void (*on_tick)(void *, uint64_t);
    void (*on_launch_event)(void *, const PrismLaunchEventV1 *);
    void (*on_instance_event)(void *, const PrismInstanceEventV1 *);
    void (*on_theme_event)(void *, const PrismThemeEventV1 *);
    void (*on_work_completed)(void *, const PrismWorkCompletionV1 *);
    void (*on_gesture)(void *, const PrismGestureEventV1 *);
    void (*on_layout_state)(void *, const PrismLayoutStateV1 *);
    void (*on_layout_control_result)(void *, const PrismLayoutControlResultV1 *);
    void (*on_text_edit)(void *, PrismStringViewV1, PrismStringViewV1);
    int32_t (*on_close_requested)(void *);
    void (*on_control_value)(void *, const PrismControlValueEventV1 *);
};

_Static_assert(sizeof(struct Prefix) == offsetof(PrismAppModuleV1, on_task_completed),
               "The old prefix ends at the new task callback");

static void *mapping;
static size_t page;
static const PrismHostApiV1 *host;

static void *Create(const PrismAppInitV1 *init)
{
    host = init->host;
    return (void *)init->host;
}

static void Destroy(void *instance)
{
    (void)instance;
}

PRISM_APP_EXPORT const PrismAppModuleV1 *prism_app_module_v1(void)
{
    if (!mapping) {
        page = (size_t)sysconf(_SC_PAGESIZE);
        mapping = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED || mprotect((char *)mapping + page, page, PROT_NONE)) {
            abort();
        }

        struct Prefix prefix = {0};
        prefix.struct_size = sizeof(prefix);
#ifndef TEST_OWNER_TASK_EXACT_PREFIX
        prefix.struct_size += sizeof(((PrismAppModuleV1 *)0)->on_task_completed) - 1;
#endif
        prefix.abi_version = PRISM_APP_ABI_V1;
        prefix.create = Create;
        prefix.destroy = Destroy;
        memcpy((char *)mapping + page - sizeof(prefix), &prefix, sizeof(prefix));
    }
    return (const PrismAppModuleV1 *)((char *)mapping + page - sizeof(struct Prefix));
}

const PrismHostApiV1 *owner_task_fixture_host(void)
{
    return host;
}

__attribute__((destructor)) static void Unload(void)
{
    if (mapping && mapping != MAP_FAILED) {
        munmap(mapping, page * 2);
    }
}
