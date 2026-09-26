#include "prism/app/module_support.hpp"
namespace {
void* Create(const PrismAppInitV1* init) noexcept {
    if (!prism::app::ValidHost(init) || !prism::app::Ready(init->host)) return nullptr;
    return const_cast<PrismHostApiV1*>(init->host);
}
void Destroy(void*) noexcept {}
const PrismAppModuleV1 api{sizeof(api),PRISM_APP_ABI_V1,Create,Destroy,nullptr,nullptr,nullptr,nullptr};
}
extern "C" PRISM_APP_EXPORT const PrismAppModuleV1* prism_app_module_v1() { return &api; }
