#include "prism/contracts/app_module.h"
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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
    const size_t prefix = offsetof(PrismAppModuleV1, on_control_value);
    if (!mapping) {
        page = (size_t)sysconf(_SC_PAGESIZE);
        mapping = mmap(0, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED || mprotect((char *)mapping + page, page, PROT_NONE) != 0) {
            abort();
        }
        const PrismAppModuleV1 api = {.struct_size = prefix + sizeof(api.on_control_value) - 1,
                                      .abi_version = PRISM_APP_ABI_V1,
                                      .create = Create,
                                      .destroy = Destroy};
        // The optional pointer itself is inaccessible, even though struct_size
        // advertises all but one byte. Only complete fields may be read.
        memcpy((char *)mapping + page - prefix, &api, prefix);
    }
    return (const PrismAppModuleV1 *)((char *)mapping + page - prefix);
}

__attribute__((destructor)) static void Unload(void)
{
    if (mapping && mapping != MAP_FAILED) {
        munmap(mapping, page * 2);
    }
}
