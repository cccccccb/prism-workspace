#include "notepad.hpp"
#include "prism/app/module_support.hpp"
#include <memory>

namespace {
void *Create(const PrismAppInitV1 *init) noexcept
{
    if (!prism::app::ValidHost(init) ||
        init->host->struct_size <
            offsetof(PrismHostApiV1, cancel_work) + sizeof(init->host->cancel_work) ||
        !init->host->submit_work) {
        return nullptr;
    }
    try {
        auto instance = std::make_unique<prism::notepad::Notepad>(init->host);
        prism::app::Ready(init->host);
        return instance.release();
    } catch (...) {
        return nullptr;
    }
}

void Destroy(void *instance) noexcept
{
    delete static_cast<prism::notepad::Notepad *>(instance);
}

void Action(void *instance, PrismStringViewV1 action) noexcept
{
    try {
        static_cast<prism::notepad::Notepad *>(instance)->Action({action.data, action.size});
    } catch (...) {
    }
}

void Edit(void *instance, PrismStringViewV1 action, PrismStringViewV1 text) noexcept
{
    try {
        static_cast<prism::notepad::Notepad *>(instance)->Edit({action.data, action.size},
                                                               {text.data, text.size});
    } catch (...) {
    }
}

void Complete(void *instance, const PrismWorkCompletionV1 *completion) noexcept
{
    try {
        if (completion && completion->struct_size >= sizeof(*completion)) {
            static_cast<prism::notepad::Notepad *>(instance)->Complete(*completion);
        }
    } catch (...) {
    }
}

int32_t RequestClose(void *instance) noexcept
{
    try {
        return static_cast<prism::notepad::Notepad *>(instance)->RequestClose() ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

const PrismAppModuleV1 Module{.struct_size = sizeof(PrismAppModuleV1),
                              .abi_version = PRISM_APP_ABI_V1,
                              .create = Create,
                              .destroy = Destroy,
                              .on_action = Action,
                              .on_work_completed = Complete,
                              .on_text_edit = Edit,
                              .on_close_requested = RequestClose};
} // namespace

extern "C" PRISM_APP_EXPORT const PrismAppModuleV1 *prism_app_module_v1()
{
    return &Module;
}
