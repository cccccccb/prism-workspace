#include "module_task_p.hpp"

#include <cstddef>
#include <limits>
#include <utility>

namespace prism::sdk {
namespace {
class TaskSubmission {
public:
    explicit TaskSubmission(bool &active) noexcept : active_(active)
    {
        active_ = true;
    }

    ~TaskSubmission()
    {
        active_ = false;
    }

private:
    bool &active_;
};

bool ValidView(PrismTaskStringViewV1 view, std::size_t maximum) noexcept
{
    return view.size <= maximum && (view.data || !view.size);
}

std::string Copy(PrismTaskStringViewV1 view)
{
    return view.size ? std::string(view.data, view.size) : std::string{};
}

std::optional<contracts::OwnerFileTaskOptions> CopyFile(const PrismFileTaskOptionsV1 *file)
{
    constexpr auto minimum = offsetof(PrismFileTaskOptionsV1, flags) + sizeof(file->flags);
    if (!file || file->struct_size < minimum ||
        !ValidView(file->initial_directory, contracts::kMaxOwnerFileTaskPathBytes) ||
        !ValidView(file->suggested_name, contracts::kMaxOwnerFileTaskNameBytes) ||
        file->extensions_size > contracts::kMaxOwnerFileTaskExtensions ||
        (!file->extensions && file->extensions_size) ||
        (file->flags & ~PRISM_FILE_TASK_SHOW_HIDDEN_V1)) {
        return std::nullopt;
    }
    for (std::size_t at = 0; at < file->extensions_size; ++at) {
        if (!ValidView(file->extensions[at], contracts::kMaxOwnerFileTaskExtensionBytes)) {
            return std::nullopt;
        }
    }

    contracts::OwnerFileTaskOptions result;
    result.initial_directory = Copy(file->initial_directory);
    result.suggested_name = Copy(file->suggested_name);
    result.extensions.reserve(file->extensions_size);
    for (std::size_t at = 0; at < file->extensions_size; ++at) {
        result.extensions.push_back(Copy(file->extensions[at]));
    }
    result.show_hidden = file->flags & PRISM_FILE_TASK_SHOW_HIDDEN_V1;
    return result;
}

std::optional<contracts::OwnerTaskRequest> CopyRequest(const PrismTaskRequestV1 *request)
{
    constexpr auto minimum =
        offsetof(PrismTaskRequestV1, choices_size) + sizeof(request->choices_size);
    constexpr auto file_tail = offsetof(PrismTaskRequestV1, file) + sizeof(request->file);
    if (!request || request->struct_size < minimum ||
        !contracts::OwnerTaskCapability(static_cast<contracts::OwnerTaskKind>(request->kind)) ||
        (!request->choices && request->choices_size) ||
        request->choices_size > contracts::kMaxOwnerTaskChoices ||
        !ValidView(request->title, contracts::kMaxOwnerTaskTitleBytes) ||
        !ValidView(request->message, contracts::kMaxOwnerTaskMessageBytes)) {
        return std::nullopt;
    }

    const bool confirmation = request->kind == PRISM_TASK_CONFIRMATION_V1;
    if ((confirmation &&
         (!request->choices_size || (request->struct_size >= file_tail && request->file))) ||
        (!confirmation &&
         (request->choices_size || request->struct_size < file_tail || !request->file))) {
        return std::nullopt;
    }
    for (std::size_t at = 0; at < request->choices_size; ++at) {
        const auto &choice = request->choices[at];
        if (choice.struct_size < sizeof(PrismTaskChoiceV1) ||
            !ValidView(choice.label, contracts::kMaxOwnerTaskChoiceLabelBytes)) {
            return std::nullopt;
        }
    }

    contracts::OwnerTaskRequest result;
    result.request_id = 1; // Provisional ID for complete value validation before allocation.
    result.kind = static_cast<contracts::OwnerTaskKind>(request->kind);
    result.title = Copy(request->title);
    result.message = Copy(request->message);
    result.choices.reserve(request->choices_size);
    for (std::size_t at = 0; at < request->choices_size; ++at) {
        const auto &choice = request->choices[at];
        result.choices.push_back({choice.id, Copy(choice.label),
                                  static_cast<contracts::OwnerTaskChoiceRole>(choice.role)});
    }
    if (request->struct_size >= file_tail && request->file) {
        result.file = CopyFile(request->file);
        if (!result.file) {
            return std::nullopt;
        }
    }
    if (!contracts::ValidateOwnerTaskRequest(result)) {
        return std::nullopt;
    }
    return result;
}

void DropPending(ModuleTaskState &state, std::uint64_t request) noexcept
{
    if (state.pending && state.pending->request_id == request) {
        state.pending.reset();
        state.cancel_requested = false;
    }
}
} // namespace

bool ModuleSession::SupportsOwnerTasks() const noexcept
{
    return OnOwnerThread() && !closed_ && module_.Api().on_task_completed;
}

std::uint32_t ModuleSession::TaskCapabilities(void *context) noexcept
{
    if (!context || !static_cast<ModuleSession *>(context)->OnOwnerThread()) {
        return 0;
    }
    try {
        auto &self = *static_cast<ModuleSession *>(context);
        const auto &tasks = *self.tasks_;
        if (!self.SupportsOwnerTasks() || !tasks.submit || !tasks.cancel || !tasks.capabilities) {
            return 0;
        }
        const auto capabilities = tasks.capabilities();
        return self.closed_ ? 0 : capabilities & contracts::kOwnerTaskCapabilities;
    } catch (...) {
        return 0;
    }
}

std::uint64_t ModuleSession::RequestTask(void *context, const PrismTaskRequestV1 *request) noexcept
{
    if (!context || !static_cast<ModuleSession *>(context)->OnOwnerThread()) {
        return 0;
    }
    auto &self = *static_cast<ModuleSession *>(context);
    auto &tasks = *self.tasks_;
    if (self.closed_ || !self.instance_ || !self.started_ || tasks.submitting || tasks.pending ||
        tasks.last_request == std::numeric_limits<std::uint64_t>::max()) {
        return 0;
    }

    std::uint64_t id{};
    try {
        const TaskSubmission submitting(tasks.submitting);
        auto owned = CopyRequest(request);
        if (!owned || !(TaskCapabilities(context) & contracts::OwnerTaskCapability(owned->kind))) {
            return 0;
        }
        owned->request_id = tasks.last_request + 1;
        tasks.pending = *owned;
        id = owned->request_id;
        tasks.last_request = id; // Consumed IDs are never reused, including rejected staging.
        tasks.cancel_requested = false;
        if (!tasks.submit(*owned) || self.closed_ || !tasks.pending ||
            tasks.pending->request_id != id) {
            DropPending(tasks, id);
            return 0;
        }
        return id;
    } catch (...) {
        // A throwing provider may have staged before failing. Best-effort revoke
        // its exact correlation; no terminal callback is delivered here.
        if (id && !self.closed_ && tasks.pending && tasks.pending->request_id == id) {
            try {
                const TaskSubmission submitting(tasks.submitting);
                tasks.cancel(id);
            } catch (...) {
            }
            DropPending(tasks, id);
        }
        return 0;
    }
}

std::int32_t ModuleSession::CancelTask(void *context, std::uint64_t request) noexcept
{
    if (!context || !static_cast<ModuleSession *>(context)->OnOwnerThread()) {
        return -1;
    }
    auto &self = *static_cast<ModuleSession *>(context);
    auto &tasks = *self.tasks_;
    if (self.closed_ || !self.instance_ || tasks.submitting || !tasks.cancel || !tasks.pending ||
        !request || tasks.pending->request_id != request) {
        return -1;
    }
    if (tasks.cancel_requested) {
        return 0;
    }

    try {
        const TaskSubmission submitting(tasks.submitting);
        if (!tasks.cancel(request) || self.closed_) {
            return -1;
        }
        if (tasks.pending && tasks.pending->request_id == request) {
            tasks.cancel_requested = true;
        }
        return 0;
    } catch (...) {
        return -1;
    }
}

bool ModuleSession::DeliverTaskResult(const contracts::OwnerTaskResult &result)
{
    if (!OnOwnerThread() || closed_ || !instance_ || !module_.Api().on_task_completed ||
        tasks_->submitting || !tasks_->pending ||
        !contracts::ValidateOwnerTaskResult(result, *tasks_->pending) ||
        (tasks_->cancel_requested && result.outcome == contracts::OwnerTaskOutcome::Success)) {
        return false;
    }

    // Copy before retiring the pending request. The callback may replace Host
    // controller state or synchronously submit another task.
    const auto owned = result;
    DropPending(*tasks_, owned.request_id);
    const PrismTaskResultV1 projected{sizeof(projected),
                                      owned.request_id,
                                      static_cast<std::uint32_t>(owned.outcome),
                                      owned.choice_id,
                                      static_cast<std::uint32_t>(owned.cancel_reason),
                                      static_cast<std::uint32_t>(owned.failure_code),
                                      {owned.diagnostic.data(), owned.diagnostic.size()},
                                      {owned.file_path.data(), owned.file_path.size()},
                                      owned.overwrite_approved ? 1u : 0u};
    module_.Api().on_task_completed(instance_, &projected);
    return true;
}
} // namespace prism::sdk
