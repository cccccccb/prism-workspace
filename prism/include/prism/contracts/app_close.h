#ifndef PRISM_CONTRACTS_APP_CLOSE_H
#define PRISM_CONTRACTS_APP_CLOSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum PrismCloseDecisionV1 {
    PRISM_CLOSE_REJECT_V1 = 0,
    PRISM_CLOSE_ACCEPT_V1 = 1,
    PRISM_CLOSE_DEFER_V1 = 2
};

enum PrismCloseCompletionResultV1 {
    PRISM_CLOSE_COMPLETED_V1 = 0,
    PRISM_CLOSE_INVALID_V1 = -1,
    PRISM_CLOSE_CLOSED_V1 = -2,
    PRISM_CLOSE_WRONG_THREAD_V1 = -3
};

/* Borrowed during on_close_request only. The Host issues nonzero IDs and never
 * reuses one within a module instance. Repeated platform close requests while
 * deferred do not create additional callbacks or IDs. */
typedef struct PrismCloseRequestV1 {
    uint32_t struct_size;
    uint64_t request_id;
} PrismCloseRequestV1;

#ifdef __cplusplus
}
#endif
#endif
