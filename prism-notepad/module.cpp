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
        static_cast<prism::notepad::Notepad *>(instance)->Recover();
    }
}

void Edit(void *instance, PrismStringViewV1 action, PrismStringViewV1 text) noexcept
{
    try {
        static_cast<prism::notepad::Notepad *>(instance)->Edit({action.data, action.size},
                                                               {text.data, text.size});
    } catch (...) {
        static_cast<prism::notepad::Notepad *>(instance)->Recover();
    }
}

void Complete(void *instance, const PrismWorkCompletionV1 *completion) noexcept
{
    try {
        if (completion && completion->struct_size >= sizeof(*completion)) {
            static_cast<prism::notepad::Notepad *>(instance)->Complete(*completion);
        }
    } catch (...) {
        static_cast<prism::notepad::Notepad *>(instance)->Recover();
    }
}

int32_t RequestClose(void *instance) noexcept
{
    try {
        return static_cast<prism::notepad::Notepad *>(instance)->RequestLegacyClose() ? 1 : 0;
    } catch (...) {
        static_cast<prism::notepad::Notepad *>(instance)->Recover();
        return 0;
    }
}

void TaskComplete(void *instance, const PrismTaskResultV1 *result) noexcept
{
    try {
        if (result && result->struct_size >=
                          offsetof(PrismTaskResultV1, diagnostic) + sizeof(result->diagnostic)) {
            static_cast<prism::notepad::Notepad *>(instance)->TaskComplete(*result);
        }
    } catch (...) {
        static_cast<prism::notepad::Notepad *>(instance)->Recover();
    }
}

int32_t CloseRequest(void *instance, const PrismCloseRequestV1 *request) noexcept
{
    try {
        if (request && request->struct_size >= sizeof(*request)) {
            return static_cast<prism::notepad::Notepad *>(instance)->RequestClose(*request);
        }
    } catch (...) {
        static_cast<prism::notepad::Notepad *>(instance)->Recover();
    }
    return PRISM_CLOSE_REJECT_V1;
}

void FeedbackAction(void *instance, const PrismFeedbackActionEventV1 *event) noexcept
{
    try {
        if (event && event->struct_size >= sizeof(*event)) {
            static_cast<prism::notepad::Notepad *>(instance)->FeedbackAction(*event);
        }
    } catch (...) {
        static_cast<prism::notepad::Notepad *>(instance)->Recover();
    }
}

const PrismAppModuleV1 Module{.struct_size = sizeof(PrismAppModuleV1),
                              .abi_version = PRISM_APP_ABI_V1,
                              .create = Create,
                              .destroy = Destroy,
                              .on_action = Action,
                              .on_work_completed = Complete,
                              .on_text_edit = Edit,
                              .on_close_requested = RequestClose,
                              .on_task_completed = TaskComplete,
                              .on_close_request = CloseRequest,
                              .on_feedback_action = FeedbackAction};
} // namespace

extern "C" PRISM_APP_EXPORT const PrismAppModuleV1 *prism_app_module_v1()
{
    return &Module;
}
