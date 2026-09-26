#ifndef PRISM_CONTRACTS_APP_MODULE_H
#define PRISM_CONTRACTS_APP_MODULE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PRISM_APP_ABI_V1 1u
#define PRISM_APP_ENTRY_V1 "prism_app_module_v1"

/* In-process C ABI. These pointers/structs are never serialized as IPC.
 * Strings are borrowed UTF-8 byte slices, not necessarily NUL terminated.
 * Callbacks run on the host event thread; modules must not throw across ABI.
 * The module receives state/action APIs, never Wayland/Skia objects. */
typedef struct PrismStringViewV1 {
    const char* data;
    size_t size;
} PrismStringViewV1;

enum PrismValueKindV1 {
    PRISM_VALUE_STRING_V1 = 1,
    PRISM_VALUE_NUMBER_V1 = 2,
    PRISM_VALUE_BOOL_V1 = 3
};
typedef struct PrismValueV1 {
    uint32_t kind;
    union {
        PrismStringViewV1 string;
        double number;
        uint32_t boolean;
    } as;
} PrismValueV1;

typedef struct PrismHostApiV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    void* context;
    /* Host copies values before returning. Zero means accepted. */
    int32_t (*set_binding)(void*, PrismStringViewV1, PrismValueV1);
    int32_t (*backend_ready)(void*);
    /* Queue an asynchronous platform launch request; zero means rejected.
     * An accepted request ID does not imply that a window was presented. */
    uint64_t (*launch_app)(void*, PrismStringViewV1);
    /* One-shot tick, delay in monotonic nanoseconds; zero means accepted. */
    int32_t (*schedule_tick)(void*, uint64_t);
} PrismHostApiV1;

typedef struct PrismAppInitV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t instance_id;
    PrismStringViewV1 app_id;
    /* Valid for this module instance until destroy() returns. */
    const PrismHostApiV1* host;
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

typedef struct PrismAppModuleV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    /* create returns NULL on failure; destroy is required. */
    void* (*create)(const PrismAppInitV1*);
    void (*destroy)(void*);
    /* Optional. All arguments are borrowed for the call only. */
    void (*on_action)(void*, PrismStringViewV1);
    void (*on_tick)(void*, uint64_t monotonic_ns);
    void (*on_launch_event)(void*, const PrismLaunchEventV1*);
} PrismAppModuleV1;

typedef const PrismAppModuleV1* (*PrismAppEntryV1)(void);
#if defined(__GNUC__) || defined(__clang__)
#define PRISM_APP_EXPORT __attribute__((visibility("default")))
#else
#define PRISM_APP_EXPORT
#endif
PRISM_APP_EXPORT const PrismAppModuleV1* prism_app_module_v1(void);

#ifdef __cplusplus
}
#endif
#endif
