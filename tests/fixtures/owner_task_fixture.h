#ifndef PRISM_TEST_OWNER_TASK_FIXTURE_H
#define PRISM_TEST_OWNER_TASK_FIXTURE_H

#include "prism/contracts/app_module.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OwnerTaskFixtureRecord {
    uint64_t request_id;
    uint32_t outcome;
    uint32_t choice_id;
    uint32_t cancel_reason;
    uint32_t failure_code;
    size_t diagnostic_size;
    char diagnostic[1025];
    size_t file_path_size;
    char file_path[4097];
    uint32_t overwrite_approved;
} OwnerTaskFixtureRecord;

const PrismHostApiV1 *owner_task_fixture_host(void);
uint64_t owner_task_fixture_create_request(void);
size_t owner_task_fixture_count(void);
const OwnerTaskFixtureRecord *owner_task_fixture_record(size_t index);
void owner_task_fixture_reenter(uint32_t enabled);
uint64_t owner_task_fixture_next_request(void);
unsigned owner_task_fixture_errors(void);

#ifdef __cplusplus
}
#endif
#endif
