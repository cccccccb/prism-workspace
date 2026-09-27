#include "prism/contracts/app_module.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* The new pointer is inaccessible. A reported partial tail must never cause
 * the loader to read any part of it or install it as a callable function. */
struct Prefix {
    uint32_t struct_size, abi_version;
    void *(*create)(const PrismAppInitV1 *);
    void (*destroy)(void *);
    void (*on_action)(void *, PrismStringViewV1);
    void (*on_tick)(void *, uint64_t);
    void (*on_launch_event)(void *, const PrismLaunchEventV1 *);
    void (*on_instance_event)(void *, const PrismInstanceEventV1 *);
    void (*on_theme_event)(void *, const PrismThemeEventV1 *);
};

_Static_assert(sizeof(struct Prefix) == offsetof(PrismAppModuleV1, on_work_completed),
               "Prefix must end at the new work callback");
static void *mapping;
static size_t page;

static void *Create(const PrismAppInitV1 *init)
{
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
        mapping = mmap(0, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED || mprotect((char *)mapping + page, page, PROT_NONE) != 0) {
            abort();
        }
        const struct Prefix prefix = {offsetof(PrismAppModuleV1, on_work_completed) +
                                          sizeof(void *) - 1,
                                      PRISM_APP_ABI_V1,
                                      Create,
                                      Destroy,
                                      0,
                                      0,
                                      0,
                                      0,
                                      0};
        memcpy((char *)mapping + page - sizeof(prefix), &prefix, sizeof(prefix));
    }
    return (const PrismAppModuleV1 *)((char *)mapping + page - sizeof(struct Prefix));
}

__attribute__((destructor)) static void Unload(void)
{
    if (mapping && mapping != MAP_FAILED) {
        munmap(mapping, page * 2);
    }
}
