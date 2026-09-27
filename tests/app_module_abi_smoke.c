#include "prism/contracts/app_module.h"
_Static_assert(sizeof(((PrismAppInitV1 *)0)->instance_id) == 8, "Stable ID width");
_Static_assert(sizeof(((PrismHostApiV1 *)0)->abi_version) == 4, "Stable version width");

int main(void)
{
    return PRISM_APP_ABI_V1 != 1u;
}
