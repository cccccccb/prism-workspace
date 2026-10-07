#include "feedback_fixture.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

_Static_assert(offsetof(PrismHostApiV1, feedback_capabilities) ==
                   offsetof(PrismHostApiV1, complete_close) +
                       sizeof(((PrismHostApiV1 *)0)->complete_close),
               "Feedback appends to the prior Host prefix");
_Static_assert(offsetof(PrismAppModuleV1, on_feedback_action) ==
                   offsetof(PrismAppModuleV1, on_close_request) +
                       sizeof(((PrismAppModuleV1 *)0)->on_close_request),
               "Feedback appends to the prior module prefix");

static const PrismHostApiV1 *host;
static uint64_t create_request, next_request, destroy_request;
static uint32_t create_capabilities, reenter;
static int32_t destroy_dismiss;
static unsigned count;
static struct FeedbackFixtureRecord records[16];
#ifdef TEST_FEEDBACK_PREFIX
static void *mapping;
static size_t page;
#endif

static uint64_t Show(void)
{
    const PrismFeedbackRequestV1 request = {sizeof(request),
                                            PRISM_FEEDBACK_INFO_V1,
                                            {"Callback feedback", 17},
                                            {"Ready", 5},
                                            NULL,
                                            0,
                                            4000};
    return host->show_feedback(host->context, &request);
}

static void *Create(const PrismAppInitV1 *init)
{
    if (!init || !init->host ||
        init->host->struct_size <
            offsetof(PrismHostApiV1, dismiss_feedback) + sizeof(init->host->dismiss_feedback) ||
        !init->host->show_feedback || !init->host->feedback_capabilities ||
        !init->host->dismiss_feedback) {
        return NULL;
    }
    host = init->host;
    count = reenter = 0;
    next_request = destroy_request = 0;
    destroy_dismiss = 99;
    memset(records, 0, sizeof(records));
    create_capabilities = host->feedback_capabilities(host->context);
    create_request = Show();
    return (void *)host;
}

static void Destroy(void *instance)
{
    (void)instance;
    destroy_request = Show();
    destroy_dismiss = host->dismiss_feedback(host->context, 1);
}

#ifndef TEST_FEEDBACK_NO_ACTION
static void Action(void *instance, const PrismFeedbackActionEventV1 *event)
{
    (void)instance;
    if (!event || event->struct_size < sizeof(*event) || count >= 16) {
        abort();
    }
    records[count++] =
        (struct FeedbackFixtureRecord){event->struct_size, event->request_id, event->action_id};
    if (reenter) {
        reenter = 0;
        next_request = Show();
    }
}
#endif

static const PrismAppModuleV1 module = {.struct_size = sizeof(module),
                                        .abi_version = PRISM_APP_ABI_V1,
                                        .create = Create,
                                        .destroy = Destroy,
#ifndef TEST_FEEDBACK_NO_ACTION
                                        .on_feedback_action = Action
#endif
};

PRISM_APP_EXPORT const PrismAppModuleV1 *prism_app_module_v1(void)
{
#ifdef TEST_FEEDBACK_PREFIX
    if (!mapping) {
        page = (size_t)sysconf(_SC_PAGESIZE);
        mapping = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED || mprotect((char *)mapping + page, page, PROT_NONE)) {
            abort();
        }
        const size_t prefix = offsetof(PrismAppModuleV1, on_feedback_action);
        memcpy((char *)mapping + page - prefix, &module, prefix);
        PrismAppModuleV1 *old = (PrismAppModuleV1 *)((char *)mapping + page - prefix);
        old->struct_size = (uint32_t)prefix;
#ifdef TEST_FEEDBACK_PARTIAL
        old->struct_size += sizeof(module.on_feedback_action) - 1;
#endif
    }
    return (const PrismAppModuleV1 *)((char *)mapping + page -
                                      offsetof(PrismAppModuleV1, on_feedback_action));
#else
    return &module;
#endif
}

const PrismHostApiV1 *feedback_fixture_host(void)
{
    return host;
}

uint64_t feedback_fixture_create_request(void)
{
    return create_request;
}

uint32_t feedback_fixture_create_capabilities(void)
{
    return create_capabilities;
}

unsigned feedback_fixture_count(void)
{
    return count;
}

const struct FeedbackFixtureRecord *feedback_fixture_record(unsigned index)
{
    return index < count ? &records[index] : NULL;
}

void feedback_fixture_reenter(uint32_t enabled)
{
    reenter = enabled;
}

uint64_t feedback_fixture_next_request(void)
{
    return next_request;
}

uint64_t feedback_fixture_destroy_request(void)
{
    return destroy_request;
}

int32_t feedback_fixture_destroy_dismiss(void)
{
    return destroy_dismiss;
}

#ifdef TEST_FEEDBACK_PREFIX
__attribute__((destructor)) static void Unload(void)
{
    if (mapping && mapping != MAP_FAILED) {
        munmap(mapping, page * 2);
    }
}
#endif
