#ifndef PRISM_TEST_FEEDBACK_FIXTURE_H
#define PRISM_TEST_FEEDBACK_FIXTURE_H
#include "prism/contracts/app_module.h"
#ifdef __cplusplus
extern "C" {
#endif
struct FeedbackFixtureRecord {
    uint32_t struct_size;
    uint64_t request_id;
    uint32_t action_id;
};

const PrismHostApiV1 *feedback_fixture_host(void);
uint64_t feedback_fixture_create_request(void);
uint32_t feedback_fixture_create_capabilities(void);
unsigned feedback_fixture_count(void);
const struct FeedbackFixtureRecord *feedback_fixture_record(unsigned index);
void feedback_fixture_reenter(uint32_t enabled);
uint64_t feedback_fixture_next_request(void);
uint64_t feedback_fixture_destroy_request(void);
int32_t feedback_fixture_destroy_dismiss(void);
#ifdef __cplusplus
}
#endif
#endif
