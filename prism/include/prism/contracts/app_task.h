#ifndef PRISM_CONTRACTS_APP_TASK_H
#define PRISM_CONTRACTS_APP_TASK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* In-process owner-thread ABI, never an IPC representation. Every string and
 * array is borrowed for the call only. The Host copies accepted requests.
 * A request contains no owner identity, Scene object or authorization token. */
typedef struct PrismTaskStringViewV1 {
    const char *data;
    size_t size;
} PrismTaskStringViewV1;

enum PrismTaskCapabilityV1 {
    PRISM_TASK_CAP_CONFIRMATION_V1 = 1u << 0,
    PRISM_TASK_CAP_OPEN_FILE_V1 = 1u << 1,
    PRISM_TASK_CAP_SAVE_FILE_V1 = 1u << 2,
    PRISM_TASK_CAP_SELECT_DIRECTORY_V1 = 1u << 3
};

enum PrismTaskKindV1 {
    PRISM_TASK_CONFIRMATION_V1 = 1,
    PRISM_TASK_OPEN_FILE_V1 = 2,
    PRISM_TASK_SAVE_FILE_V1 = 3,
    PRISM_TASK_SELECT_DIRECTORY_V1 = 4
};

enum PrismTaskChoiceRoleV1 {
    PRISM_TASK_CHOICE_SECONDARY_V1 = 0,
    PRISM_TASK_CHOICE_PRIMARY_V1 = 1,
    PRISM_TASK_CHOICE_DESTRUCTIVE_V1 = 2
};

typedef struct PrismTaskChoiceV1 {
    uint32_t struct_size;
    uint32_t id; /* Nonzero and unique within this request. */
    PrismTaskStringViewV1 label;
    uint32_t role;
} PrismTaskChoiceV1;

enum PrismFileTaskFlagsV1 { PRISM_FILE_TASK_SHOW_HIDDEN_V1 = 1u << 0 };

/* File requests have no business choices. Empty initial_directory means Host
 * home; otherwise it is an absolute directory hint resolved by the provider.
 * suggested_name is SaveFile-only and a single Linux filename of at most 255
 * bytes. Extensions are case-sensitive ASCII suffixes such as .md or .tar.gz,
 * at most 16 entries of 32 bytes each; empty means all files. Directory tasks
 * have no suggested_name or extensions. Paths are limited to 4096 bytes. */
typedef struct PrismFileTaskOptionsV1 {
    uint32_t struct_size;
    PrismTaskStringViewV1 initial_directory;
    PrismTaskStringViewV1 suggested_name;
    const PrismTaskStringViewV1 *extensions;
    size_t extensions_size;
    uint32_t flags;
} PrismFileTaskOptionsV1;

/* Confirmation accepts one or two business choices. The shared panel always
 * adds its own Cancel button; cancellation is not a successful business choice.
 * title/label are nonempty single-line UTF-8. message may contain LF and TAB.
 * Limits in bytes: title 256, message 2048, each label 96, total text 4096. */
typedef struct PrismTaskRequestV1 {
    uint32_t struct_size;
    uint32_t kind;
    PrismTaskStringViewV1 title;
    PrismTaskStringViewV1 message;
    const PrismTaskChoiceV1 *choices;
    size_t choices_size;
    /* Additive tail. Confirmation accepts the original prefix without this
     * field. Read only when struct_size covers the complete pointer. */
    const PrismFileTaskOptionsV1 *file;
} PrismTaskRequestV1;

enum PrismTaskOutcomeV1 {
    PRISM_TASK_SUCCEEDED_V1 = 0,
    PRISM_TASK_CANCELLED_V1 = 1,
    PRISM_TASK_FAILED_V1 = 2
};

enum PrismTaskCancelReasonV1 {
    PRISM_TASK_CANCEL_NONE_V1 = 0,
    PRISM_TASK_CANCEL_USER_V1 = 1,
    PRISM_TASK_CANCEL_ESCAPE_V1 = 2,
    PRISM_TASK_CANCEL_UI_REPLACED_V1 = 3,
    PRISM_TASK_CANCEL_UNAVAILABLE_V1 = 4,
    PRISM_TASK_CANCEL_FRONTEND_FAILED_V1 = 5
};

enum PrismTaskFailureCodeV1 {
    PRISM_TASK_FAILURE_NONE_V1 = 0,
    PRISM_TASK_PREPARATION_FAILED_V1 = 1,
    PRISM_TASK_OPERATION_FAILED_V1 = 2,
    PRISM_TASK_PROVIDER_UNAVAILABLE_V1 = 3
};

/* Confirmation success carries only a known choice_id. File success carries
 * only file_path and SaveFile may also carry overwrite_approved=1. The path is
 * a normalized absolute UTF-8 path, not a file handle or authorization token.
 * Cancelled carries only a nonzero
 * cancel_reason. Failed carries only a nonzero failure_code and optional UTF-8
 * diagnostic of at most 1024 bytes. request_id is Host-issued correlation, not
 * a work task ID or an owner permission. Views expire on callback return. */
typedef struct PrismTaskResultV1 {
    uint32_t struct_size;
    uint64_t request_id;
    uint32_t outcome;
    uint32_t choice_id;
    uint32_t cancel_reason;
    uint32_t failure_code;
    PrismTaskStringViewV1 diagnostic;
    /* Additive tail. Older callbacks only read through diagnostic. */
    PrismTaskStringViewV1 file_path;
    uint32_t overwrite_approved; /* Boolean: 0 or 1; SaveFile success only. */
} PrismTaskResultV1;

#ifdef __cplusplus
}
#endif
#endif
