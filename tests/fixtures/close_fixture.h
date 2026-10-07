#ifndef PRISM_TEST_CLOSE_FIXTURE_H
#define PRISM_TEST_CLOSE_FIXTURE_H
#include "prism/contracts/app_module.h"
#ifdef __cplusplus
extern "C" {
#endif
const PrismHostApiV1 *close_fixture_host(void);
void close_fixture_mode(int32_t decision, int32_t synchronous_decision);
uint64_t close_fixture_request(void);
unsigned close_fixture_calls(void);
int32_t close_fixture_create_completion(void);
int32_t close_fixture_synchronous_completion(void);
int32_t close_fixture_duplicate_completion(void);
int32_t close_fixture_destroy_completion(void);
#ifdef __cplusplus
}
#endif
#endif
