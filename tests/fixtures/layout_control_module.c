#include "prism/contracts/app_module.h"
#include <stdlib.h>

struct Instance {
    const PrismHostApiV1 *host;
    PrismLayoutTargetV1 target;
    uint32_t result_count;
};

static void number(struct Instance *self, const char *key, size_t size, double value)
{
    PrismValueV1 property = {.kind = PRISM_VALUE_NUMBER_V1};
    property.as.number = value;
    self->host->set_binding(self->host->context, (PrismStringViewV1){key, size}, property);
}

static void *create(const PrismAppInitV1 *init)
{
    if (init->host->struct_size < sizeof(PrismHostApiV1) || !init->host->subscribe_layout ||
        !init->host->control_gesture) {
        return NULL;
    }
    struct Instance *self = calloc(1, sizeof(*self));
    if (!self) {
        return NULL;
    }
    self->host = init->host;
    self->host->subscribe_layout(self->host->context, 1);
    self->host->backend_ready(self->host->context);
    return self;
}

static void destroy(void *context)
{
    free(context);
}

static void layout(void *context, const PrismLayoutStateV1 *state)
{
    struct Instance *self = context;
    number(self, "layout_status", 13, state->status);
    number(self, "output_count", 12, state->outputs_size);
    number(self, "node_count", 10, state->nodes_size);
    const PrismLayoutControlHandleV1 *handle = NULL;
    if (state->struct_size >=
            offsetof(PrismLayoutStateV1, control_handle) + sizeof(state->control_handle) &&
        state->control_handle &&
        state->control_handle->struct_size >= sizeof(PrismLayoutControlHandleV1)) {
        handle = state->control_handle;
    }
    number(self, "handle_available", sizeof("handle_available") - 1, handle != NULL);
    if (handle) {
        number(self, "handle_boundary", sizeof("handle_boundary") - 1, handle->boundary);
        number(self, "handle_visible", sizeof("handle_visible") - 1, handle->visible);
        number(self, "handle_x", sizeof("handle_x") - 1, handle->bounds.x);
        number(self, "handle_width", sizeof("handle_width") - 1, handle->bounds.width);
    }

    if (state->workspaces_size) {
        const PrismLayoutWorkspaceV1 *workspace = state->workspaces;
        self->target = (PrismLayoutTargetV1){
            state->wm_session,        workspace->output,     workspace->id, workspace->root, 0,
            state->topology_revision, state->layout_revision};
    }
}

static void gesture(void *context, const PrismGestureEventV1 *event)
{
    struct Instance *self = context;
    number(self, "gesture_phase", 13, event->phase);
    number(self, "gesture_serial", 14, event->serial);
    if (event->phase == PRISM_GESTURE_BEGIN_V1 || event->phase == PRISM_GESTURE_END_V1) {
        PrismLayoutCommandV1 command = {0};
        command.struct_size = sizeof(command);
        command.gesture_id = event->gesture_id;
        command.phase = event->phase;
        command.operation = PRISM_LAYOUT_GROUP_GESTURE_V1;
        command.target = self->target;
        if (event->phase == PRISM_GESTURE_BEGIN_V1) {
            command.phase = 256;
            number(self, "bad_phase", 9,
                   self->host->control_gesture(self->host->context, &command));
            command.phase = event->phase;
        } else {
            command.intent = PRISM_LAYOUT_APPLY_BOUNDARY_V1;
            number(self, "bad_intent", 10,
                   self->host->control_gesture(self->host->context, &command));
            command.intent = PRISM_LAYOUT_INTENT_NONE_V1;
        }
        number(self, "command_result", 14,
               self->host->control_gesture(self->host->context, &command));
    }
}

static void control_result(void *context, const PrismLayoutControlResultV1 *event)
{
    struct Instance *self = context;
    number(self, "result_count", 12, ++self->result_count);
    number(self, "result_status", 13, event->status);
    number(self, "result_error", 12, event->error);
    number(self, "result_applied", 14, event->applied);
}

static const PrismAppModuleV1 api = {.struct_size = sizeof(api),
                                     .abi_version = PRISM_APP_ABI_V1,
                                     .create = create,
                                     .destroy = destroy,
                                     .on_gesture = gesture,
                                     .on_layout_state = layout,
                                     .on_layout_control_result = control_result};

PRISM_APP_EXPORT const PrismAppModuleV1 *prism_app_module_v1(void)
{
    return &api;
}
