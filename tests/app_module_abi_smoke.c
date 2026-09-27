#include "prism/contracts/app_module.h"
_Static_assert(sizeof(((PrismAppInitV1 *)0)->instance_id) == 8, "Stable ID width");
_Static_assert(sizeof(((PrismHostApiV1 *)0)->abi_version) == 4, "Stable version width");
_Static_assert(offsetof(PrismHostApiV1, submit_work) ==
                   offsetof(PrismHostApiV1, select_color_scheme) +
                       sizeof(((PrismHostApiV1 *)0)->select_color_scheme),
               "Async work must append to the old Host API prefix");
_Static_assert(offsetof(PrismAppModuleV1, on_work_completed) ==
                   offsetof(PrismAppModuleV1, on_theme_event) +
                       sizeof(((PrismAppModuleV1 *)0)->on_theme_event),
               "Work completion must append to the old module prefix");
_Static_assert(sizeof(((PrismWorkRequestV1 *)0)->task_id) == 8,
               "Async task IDs retain their fixed width");

int main(void)
{
    return PRISM_APP_ABI_V1 != 1u;
}
