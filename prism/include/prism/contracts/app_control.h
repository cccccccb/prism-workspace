#ifndef PRISM_CONTRACTS_APP_CONTROL_H
#define PRISM_CONTRACTS_APP_CONTROL_H

#include <stddef.h>
#include <stdint.h>

/* Optional in-process module ABI views. All arrays and strings are borrowed
 * only for the enclosing callback. WM identities are unrelated to Scene IDs. */
typedef struct PrismLogicalPointV1 {
    double x, y;
} PrismLogicalPointV1;

typedef struct PrismLogicalRectV1 {
    double x, y, width, height;
} PrismLogicalRectV1;

enum PrismGesturePhaseV1 {
    PRISM_GESTURE_BEGIN_V1 = 0,
    PRISM_GESTURE_UPDATE_V1 = 1,
    PRISM_GESTURE_END_V1 = 2,
    PRISM_GESTURE_CANCEL_V1 = 3
};

typedef struct PrismGestureEventV1 {
    uint32_t struct_size;
    uint32_t phase;
    uint64_t gesture_id;
    uint32_t node_index, node_generation;
    const char *action;
    size_t action_size;
    uint64_t seat, device, device_generation;
    uint32_t touch;
    int32_t contact;
    uint32_t serial;
    PrismLogicalPointV1 start, position;
    uint64_t monotonic_ns, snapshot_scene, snapshot_version;
} PrismGestureEventV1;

typedef struct PrismLayoutOutputV1 {
    uint64_t id;
    const char *name;
    size_t name_size;
    PrismLogicalRectV1 bounds;
    double scale;
    uint32_t primary, supported;
} PrismLayoutOutputV1;

typedef struct PrismLayoutWorkspaceV1 {
    uint64_t id, root, output;
    const char *name;
    size_t name_size;
    uint32_t active, mode; /* Normal = 0. */
    uint64_t mode_revision;
} PrismLayoutWorkspaceV1;

typedef struct PrismLayoutNodeV1 {
    uint64_t id, parent, workspace;
    uint32_t kind;        /* Container = 0, View = 1. */
    uint32_t arrangement; /* None, Horizontal, Vertical, Tabbed, Stacked. */
    const uint64_t *children;
    size_t children_size;
    PrismLogicalRectV1 tile_bounds, target_bounds, committed_bounds;
    double width_fraction, height_fraction;
    uint64_t instance_id;
    uint32_t focused, visible, fullscreen, has_committed;
} PrismLayoutNodeV1;

typedef struct PrismLayoutBoundaryV1 {
    uint64_t id, parent, first, second, workspace;
    uint32_t axis; /* X = 0, Y = 1. */
    PrismLogicalRectV1 bounds;
    uint32_t visible, resizable;
} PrismLayoutBoundaryV1;

typedef struct PrismLayoutStateV1 {
    uint32_t struct_size;
    uint32_t status; /* Current = 0, Denied = 1, Disconnected = 2. */
    uint64_t subscription_id;
    uint64_t wm_session, revision, topology_revision, layout_revision, focus_revision;
    uint64_t active_instance;
    const PrismLayoutOutputV1 *outputs;
    size_t outputs_size;
    const PrismLayoutWorkspaceV1 *workspaces;
    size_t workspaces_size;
    const PrismLayoutNodeV1 *nodes;
    size_t nodes_size;
    const PrismLayoutBoundaryV1 *boundaries;
    size_t boundaries_size;
} PrismLayoutStateV1;

enum PrismLayoutOperationV1 {
    PRISM_LAYOUT_GROUP_GESTURE_V1 = 0,
    PRISM_LAYOUT_BOUNDARY_GESTURE_V1 = 1
};

enum PrismLayoutIntentV1 {
    PRISM_LAYOUT_INTENT_NONE_V1 = 0,
    PRISM_LAYOUT_ENTER_IMMERSIVE_V1 = 1,
    PRISM_LAYOUT_EXIT_IMMERSIVE_V1 = 2,
    PRISM_LAYOUT_APPLY_BOUNDARY_V1 = 3
};

typedef struct PrismLayoutTargetV1 {
    uint64_t wm_session, output, workspace, root, boundary;
    uint64_t topology_revision, layout_revision;
} PrismLayoutTargetV1;

/* Begin is accepted only inside the corresponding on_gesture Begin callback.
 * Host mirrors subsequent Update/End/Cancel, including pending-ack coalescing.
 * During the End callback a matching End command selects its final intent.
 * Cancel may be requested at any time for an owned active gesture.
 * Module code never supplies serial, position, session or sequence credentials. */
typedef struct PrismLayoutCommandV1 {
    uint32_t struct_size;
    uint64_t gesture_id;
    uint32_t phase; /* Begin, End or Cancel from PrismGesturePhaseV1. */
    uint32_t operation;
    uint32_t intent;
    PrismLayoutTargetV1 target; /* Used only by Begin. */
} PrismLayoutCommandV1;

enum PrismLayoutStatusV1 {
    PRISM_LAYOUT_BEGAN_V1 = 0,
    PRISM_LAYOUT_UPDATED_V1 = 1,
    PRISM_LAYOUT_ENDED_V1 = 2,
    PRISM_LAYOUT_CANCELLED_V1 = 3,
    PRISM_LAYOUT_REJECTED_V1 = 4
};

enum PrismLayoutErrorV1 {
    PRISM_LAYOUT_ERROR_NONE_V1 = 0,
    PRISM_LAYOUT_UNAUTHORIZED_V1 = 1,
    PRISM_LAYOUT_STALE_SESSION_V1 = 2,
    PRISM_LAYOUT_STALE_TARGET_V1 = 3,
    PRISM_LAYOUT_STALE_LAYOUT_V1 = 4,
    PRISM_LAYOUT_INVALID_INPUT_V1 = 5,
    PRISM_LAYOUT_BUSY_V1 = 6,
    PRISM_LAYOUT_INVALID_SEQUENCE_V1 = 7,
    PRISM_LAYOUT_UNKNOWN_SESSION_V1 = 8,
    PRISM_LAYOUT_UNSUPPORTED_V1 = 9,
    PRISM_LAYOUT_EXPIRED_V1 = 10,
    PRISM_LAYOUT_DISCONNECTED_V1 = 11
};

typedef struct PrismLayoutControlResultV1 {
    uint32_t struct_size;
    uint64_t request_id, gesture_id, session_id, sequence;
    uint32_t status, error;
    uint64_t revision, topology_revision, layout_revision;
    PrismLogicalPointV1 position;
    uint32_t applied;
} PrismLayoutControlResultV1;

#endif
