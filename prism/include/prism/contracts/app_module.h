#ifndef PRISM_CONTRACTS_APP_MODULE_H
#define PRISM_CONTRACTS_APP_MODULE_H

#include "prism/contracts/app_control.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PRISM_APP_ABI_V1 1u
#define PRISM_APP_ENTRY_V1 "prism_app_module_v1"

/* In-process C ABI. These pointers/structs are never serialized as IPC.
 * Strings are borrowed UTF-8 byte slices, not necessarily NUL terminated.
 * Lifecycle/completion callbacks run on the host event thread. Only the work
 * callback runs on a scheduler worker; modules must not throw across ABI.
 * The module receives state/action APIs, never Wayland/Skia objects. */
typedef struct PrismStringViewV1 {
    const char *data;
    size_t size;
} PrismStringViewV1;

enum PrismValueKindV1 {
    PRISM_VALUE_STRING_V1 = 1,
    PRISM_VALUE_NUMBER_V1 = 2,
    PRISM_VALUE_BOOL_V1 = 3,
    PRISM_VALUE_COLOR_V1 = 4 /* straight-alpha 0xRRGGBBAA */
};

typedef struct PrismValueV1 {
    uint32_t kind;

    union {
        PrismStringViewV1 string;
        double number;
        uint32_t boolean;
        uint32_t rgba;
    } as;
} PrismValueV1;

/* Opaque, copied task data. Encode values, never object pointers. */
typedef struct PrismBytesViewV1 {
    const uint8_t *data;
    size_t size;
} PrismBytesViewV1;

enum PrismWorkSubmitResultV1 {
    PRISM_WORK_ACCEPTED_V1 = 0,
    PRISM_WORK_BUSY_V1 = 1,
    PRISM_WORK_INVALID_V1 = -1,
    PRISM_WORK_CLOSED_V1 = -2,
    PRISM_WORK_WRONG_THREAD_V1 = -3
};

enum PrismWorkPriorityV1 { PRISM_WORK_CRITICAL_V1 = 0, PRISM_WORK_DEFERRED_V1 = 2 };

enum PrismWorkStatusV1 {
    PRISM_WORK_SUCCEEDED_V1 = 0,
    PRISM_WORK_CANCELLED_V1 = 1,
    PRISM_WORK_FAILED_V1 = 2
};

/* Borrowed only during work(). The result writer copies at most once and is
 * limited by max_result_bytes. cancellation_fd is readable when cancelled;
 * work may poll it with its own IO sources, but must not drain or close it.
 * A worker must not use PrismHostApiV1 or retain any borrowed view/context. */
typedef struct PrismWorkContextV1 {
    uint32_t struct_size;
    void *context;
    int32_t (*is_cancelled)(void *);
    int32_t (*set_result)(void *, PrismBytesViewV1);
    int32_t cancellation_fd;
} PrismWorkContextV1;

typedef int32_t (*PrismWorkFunctionV1)(const PrismWorkContextV1 *, PrismBytesViewV1);

typedef struct PrismWorkRequestV1 {
    uint32_t struct_size;
    uint64_t task_id; /* Module-local nonzero ID; busy until completion dispatch. */
    uint32_t priority;
    uint64_t reserve_bytes; /* Input + maximum result + worker scratch reservation. */
    uint64_t max_result_bytes;
    PrismBytesViewV1 input; /* Host copies before submit_work returns. */
    PrismWorkFunctionV1 work;
} PrismWorkRequestV1;

/* Borrowed during on_work_completed only. Cancellation overrides late success.
 * error_code is work()'s nonzero return value, or -1 for a runtime failure.
 * Zero return with no set_result means an empty success result. */
typedef struct PrismWorkCompletionV1 {
    uint32_t struct_size;
    uint64_t task_id;
    uint32_t status;
    int32_t error_code;
    PrismBytesViewV1 result;
    PrismStringViewV1 detail;
} PrismWorkCompletionV1;

typedef struct PrismHostApiV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    void *context;
    /* Host copies values before returning. Zero means accepted. */
    int32_t (*set_binding)(void *, PrismStringViewV1, PrismValueV1);
    int32_t (*backend_ready)(void *);
    /* Queue an asynchronous platform launch request; zero means rejected.
     * An accepted request ID does not imply that a window was presented. */
    uint64_t (*launch_app)(void *, PrismStringViewV1);
    /* One-shot tick, delay in monotonic nanoseconds; zero means accepted. */
    int32_t (*schedule_tick)(void *, uint64_t);
    /* Optional tail: subscribe once to mapped ordinary window instances. */
    uint64_t (*subscribe_instances)(void *);
    /* Optional tail: select an installed theme ID, or query with an empty ID.
     * Nonzero only means queued; the theme event reports the platform result. */
    uint64_t (*select_theme)(void *, PrismStringViewV1);
    /* Optional tail: select light/dark independently of the material theme. */
    uint64_t (*select_color_scheme)(void *, PrismStringViewV1);
    /* Optional tail. Owner-thread calls only; requires on_work_completed.
     * cancel_work keeps the ID busy until its cancelled completion is dispatched. */
    int32_t (*submit_work)(void *, const PrismWorkRequestV1 *);
    int32_t (*cancel_work)(void *, uint64_t task_id);
    /* Optional control tail. Subscription returns a queued request ID, not a
     * grant. control_gesture returns zero when accepted locally; the typed
     * result reports WM authorization. Ordinary applications receive Denied. */
    uint64_t (*subscribe_layout)(void *, uint32_t enabled);
    int32_t (*control_gesture)(void *, const PrismLayoutCommandV1 *);
} PrismHostApiV1;

typedef struct PrismAppInitV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t instance_id;
    PrismStringViewV1 app_id;
    /* Valid for this module instance until destroy() returns. */
    const PrismHostApiV1 *host;
    /* Optional tail: validated absolute package asset directory. Borrowed only
     * during create(); copy before submitting work. Not a filesystem sandbox. */
    PrismStringViewV1 assets_root;
} PrismAppInitV1;

/* Projected by the host from platform instance events; borrowed for callback.
 * milestone/error values are defined by the launch protocol v1 specification. */
typedef struct PrismLaunchEventV1 {
    uint32_t struct_size;
    uint64_t request_id;
    uint64_t instance_id;
    uint32_t pid;
    uint32_t milestone;
    uint32_t error_code;
    int32_t exit_code;
    PrismStringViewV1 app_id;
    PrismStringViewV1 detail;
} PrismLaunchEventV1;

typedef struct PrismInstanceEventV1 {
    uint32_t struct_size;
    uint64_t subscription_id;
    uint64_t instance_id;
    uint32_t pid;
    uint32_t change; /* Reset=0, Running=1, Stopped=2, SnapshotDone=3 */
    PrismStringViewV1 app_id;
} PrismInstanceEventV1;

typedef struct PrismThemeEventV1 {
    uint32_t struct_size;
    uint64_t request_id;
    uint64_t generation;
    uint32_t status; /* Current=0, Applied=1, Rejected=2 */
    PrismStringViewV1 id;
    PrismStringViewV1 name;
    PrismStringViewV1 detail;
    /* Optional tail; absent in older hosts means dark. */
    PrismStringViewV1 color_scheme;
} PrismThemeEventV1;

typedef struct PrismAppModuleV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    /* create returns NULL on failure; destroy is required. */
    void *(*create)(const PrismAppInitV1 *);
    void (*destroy)(void *);
    /* Optional. All arguments are borrowed for the call only. */
    void (*on_action)(void *, PrismStringViewV1);
    void (*on_tick)(void *, uint64_t monotonic_ns);
    void (*on_launch_event)(void *, const PrismLaunchEventV1 *);
    void (*on_instance_event)(void *, const PrismInstanceEventV1 *);
    void (*on_theme_event)(void *, const PrismThemeEventV1 *);
    /* Optional tail. Result handling and Host API calls stay on the owner thread.
     * create/static constructors must return promptly. destroy runs only after
     * host-managed work has been cancelled and joined; no completion follows it. */
    void (*on_work_completed)(void *, const PrismWorkCompletionV1 *);
    /* Optional tail. All views are borrowed only during these owner callbacks. */
    void (*on_gesture)(void *, const PrismGestureEventV1 *);
    void (*on_layout_state)(void *, const PrismLayoutStateV1 *);
    void (*on_layout_control_result)(void *, const PrismLayoutControlResultV1 *);
} PrismAppModuleV1;

typedef const PrismAppModuleV1 *(*PrismAppEntryV1)(void);
#if defined(__GNUC__) || defined(__clang__)
#define PRISM_APP_EXPORT __attribute__((visibility("default")))
#else
#define PRISM_APP_EXPORT
#endif
PRISM_APP_EXPORT const PrismAppModuleV1 *prism_app_module_v1(void);

#ifdef __cplusplus
}
#endif
#endif
