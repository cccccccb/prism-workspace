#ifndef PRISM_CONTRACTS_APP_FEEDBACK_H
#define PRISM_CONTRACTS_APP_FEEDBACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum PrismFeedbackCapabilityV1 { PRISM_FEEDBACK_CAP_OWNER_V1 = 1u << 0 };

enum PrismFeedbackKindV1 {
    PRISM_FEEDBACK_INFO_V1 = 0,
    PRISM_FEEDBACK_SUCCESS_V1 = 1,
    PRISM_FEEDBACK_ERROR_V1 = 2
};

enum PrismFeedbackDismissResultV1 {
    PRISM_FEEDBACK_DISMISSED_V1 = 0,
    PRISM_FEEDBACK_INVALID_V1 = -1,
    PRISM_FEEDBACK_CLOSED_V1 = -2,
    PRISM_FEEDBACK_WRONG_THREAD_V1 = -3
};

/* Borrowed UTF-8 slices copied before show_feedback returns. Titles and labels
 * reject control characters. Messages allow LF only among control characters. */
typedef struct PrismFeedbackStringViewV1 {
    const char *data;
    size_t size;
} PrismFeedbackStringViewV1;

typedef struct PrismFeedbackActionV1 {
    uint32_t struct_size;
    uint32_t id;
    PrismFeedbackStringViewV1 label;
} PrismFeedbackActionV1;

/* Owner-local nonmodal feedback; never a permission to open files, terminate a
 * session, or invoke WM controls. One active item may replace the previous one.
 * title is nonempty, <=256 bytes; message <=2048 bytes; at most two unique nonzero
 * action IDs, nonempty labels <=48 bytes. Actions require on_feedback_action.
 * Info/Success duration is 1000..30000 ms; zero means the 4000 ms default.
 * Error duration must be zero and remains until dismissed, replaced or acted on. */
typedef struct PrismFeedbackRequestV1 {
    uint32_t struct_size;
    uint32_t kind;
    PrismFeedbackStringViewV1 title;
    PrismFeedbackStringViewV1 message;
    const PrismFeedbackActionV1 *actions;
    size_t actions_size;
    uint32_t duration_ms;
} PrismFeedbackRequestV1;

/* Borrowed during on_feedback_action. Only an explicit business action invokes
 * this callback, once; expiry, replacement and dismissal stay silent. The Host
 * retires the matching request before dispatch, allowing callback reentry. */
typedef struct PrismFeedbackActionEventV1 {
    uint32_t struct_size;
    uint64_t request_id;
    uint32_t action_id;
} PrismFeedbackActionEventV1;

#ifdef __cplusplus
}
#endif
#endif
