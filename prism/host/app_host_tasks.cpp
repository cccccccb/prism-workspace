#include "app_host_p.hpp"
#include "prism/runtime/owner_task_panel.hpp"

#include <stdexcept>
#include <utility>

namespace prism::sdk {
namespace {
contracts::OwnerTaskCancelReason CancelReason(runtime::TaskCancelReason reason)
{
    using Result = contracts::OwnerTaskCancelReason;
    switch (reason) {
    case runtime::TaskCancelReason::User:
        return Result::User;
    case runtime::TaskCancelReason::Escape:
        return Result::Escape;
    case runtime::TaskCancelReason::UiReplaced:
        return Result::UiReplaced;
    case runtime::TaskCancelReason::ScopeUnavailable:
        return Result::ScopeUnavailable;
    case runtime::TaskCancelReason::FrontendFailed:
        return Result::FrontendFailed;
    case runtime::TaskCancelReason::OwnerClosed:
        return Result::None;
    }
    throw std::logic_error("Unknown owner task cancellation");
}

contracts::OwnerTaskFailureCode FailureCode(runtime::TaskFailureCode code)
{
    using Result = contracts::OwnerTaskFailureCode;
    switch (code) {
    case runtime::TaskFailureCode::PreparationFailed:
        return Result::PreparationFailed;
    case runtime::TaskFailureCode::OperationFailed:
        return Result::OperationFailed;
    case runtime::TaskFailureCode::ProviderUnavailable:
        return Result::ProviderUnavailable;
    }
    throw std::logic_error("Unknown owner task failure");
}
} // namespace

std::uint32_t AppHost::Impl::OwnerTaskCapabilities() const
{
    if (failed || closed || !ui.master_installed || !frontend || !business ||
        !business->SupportsOwnerTasks()) {
        return 0;
    }

    std::uint32_t capabilities{};
    if (frontend->SupportsOwnerConfirmation()) {
        capabilities |= contracts::kOwnerTaskConfirmationCapability;
    }
    if (frontend->SupportsOwnerFileTasks()) {
        capabilities |= contracts::kOwnerTaskOpenFileCapability |
                        contracts::kOwnerTaskSaveFileCapability |
                        contracts::kOwnerTaskSelectDirectoryCapability;
    }
    return capabilities;
}

bool AppHost::Impl::RequestOwnerTask(const contracts::OwnerTaskRequest &request)
{
    if (!(OwnerTaskCapabilities() & contracts::OwnerTaskCapability(request.kind)) || owner_task ||
        !contracts::ValidateOwnerTaskRequest(request)) {
        return false;
    }

    OwnerTaskBinding binding;
    binding.request = request;
    owner_task = std::move(binding);
    return true;
}

bool AppHost::Impl::CancelOwnerTaskRequest(std::uint64_t request_id)
{
    if (failed || closed || !owner_task || owner_task->request.request_id != request_id ||
        owner_task->cancel_requested) {
        return false;
    }
    if (owner_task->identity) {
        const auto active = frontend->ActiveOwnerTask();
        if (!active || active->identity != *owner_task->identity) {
            return false;
        }
    }

    // Only stage intent here. In particular, cancel_task never reenters its
    // module with on_task_completed before the C ABI call returns.
    owner_task->cancel_requested = true;
    return true;
}

bool AppHost::Impl::HandleOwnerTaskAction(std::string_view action)
{
    if (!runtime::IsOwnerTaskReservedName(action)) {
        return false;
    }
    if (failed || closed || !owner_task || !owner_task->identity) {
        return true;
    }
    const auto active = frontend->ActiveOwnerTask();
    if (!active || active->identity != *owner_task->identity || owner_task->cancel_requested) {
        return true;
    }

    if (action == runtime::kOwnerTaskCancelAction) {
        frontend->CancelOwnerTask(*owner_task->identity);
        return true;
    }
    if (owner_task->request.kind != contracts::OwnerTaskKind::Confirmation) {
        return HandleFileTaskAction(action);
    }
    if (active->phase != runtime::TaskPhase::Ready) {
        return true;
    }
    for (std::size_t index = 0; index < owner_task->request.choices.size(); ++index) {
        if (action == runtime::kOwnerTaskChoiceActions[index]) {
            owner_task->choice = owner_task->request.choices[index].id;
            if (!frontend->CompleteOwnerTask(*owner_task->identity)) {
                owner_task->choice = 0;
            }
            return true;
        }
    }
    return true;
}

bool AppHost::Impl::OwnerTaskNeedsWork() const
{
    return owner_task &&
           (!owner_task->identity || owner_task->cancel_requested || !frontend->ActiveOwnerTask());
}

void AppHost::Impl::RetireOwnerTaskController()
{
    if (file_model) {
        file_model->Stop();
    }
    owner_task.reset();
    owner_task_delivered = true;
}

void AppHost::Impl::AdvanceOwnerTask()
{
    // Cancelled workers can still signal completion/capacity after the task
    // retires. Drain those signals before the Host waits again.
    if (file_model) {
        file_model->Advance();
    }

    if (failed || closed || !business || owner_task_delivered || !owner_task) {
        return;
    }

    contracts::OwnerTaskResult result;
    result.request_id = owner_task->request.request_id;
    bool complete = false;
    if (!owner_task->identity) {
        if (owner_task->cancel_requested) {
            result.outcome = contracts::OwnerTaskOutcome::Cancelled;
            result.cancel_reason = contracts::OwnerTaskCancelReason::User;
            complete = true;
        } else {
            const auto request = owner_task->request;
            std::optional<runtime::TaskIdentity> identity;
            try {
                identity = request.kind == contracts::OwnerTaskKind::Confirmation
                               ? frontend->BeginOwnerConfirmation(request)
                               : BeginFileTask(request);
            } catch (const std::exception &) {
                // Provider diagnostics remain bounded and do not expose app paths.
            }
            if (failed || closed || !owner_task ||
                owner_task->request.request_id != request.request_id) {
                return;
            }
            owner_task->identity = identity;
            if (!owner_task->identity) {
                result.outcome = contracts::OwnerTaskOutcome::Failed;
                result.failure_code = contracts::OwnerTaskFailureCode::PreparationFailed;
                result.diagnostic = "The task could not be prepared for this window";
                complete = true;
            }
        }
    }

    if (!complete && owner_task->file && !owner_task->cancel_requested) {
        AdvanceFileTask();
        if (failed || closed || !owner_task) {
            return;
        }
    }

    const bool cancellation_won = owner_task->cancel_requested;
    if (!complete && owner_task->cancel_requested) {
        frontend->CancelOwnerTask(*owner_task->identity);
        owner_task->cancel_requested = false;
    }
    if (!complete) {
        const auto terminal = frontend->TakeOwnerTaskTerminal();
        if (!terminal) {
            return;
        }
        if (terminal->identity != *owner_task->identity) {
            throw std::logic_error("Owner task terminal identity mismatch");
        }
        if (cancellation_won && terminal->cancel_reason != runtime::TaskCancelReason::OwnerClosed) {
            result.outcome = contracts::OwnerTaskOutcome::Cancelled;
            result.cancel_reason = contracts::OwnerTaskCancelReason::User;
        } else {
            switch (terminal->outcome) {
            case runtime::TaskOutcome::Success:
                result.outcome = contracts::OwnerTaskOutcome::Success;
                result.choice_id = owner_task->choice;
                result.file_path = owner_task->file_path;
                result.overwrite_approved = owner_task->overwrite_approved;
                break;
            case runtime::TaskOutcome::Cancelled:
                result.outcome = contracts::OwnerTaskOutcome::Cancelled;
                result.cancel_reason = CancelReason(*terminal->cancel_reason);
                if (result.cancel_reason == contracts::OwnerTaskCancelReason::None) {
                    RetireOwnerTaskController();
                    return;
                }
                break;
            case runtime::TaskOutcome::Failed:
                result.outcome = contracts::OwnerTaskOutcome::Failed;
                result.failure_code = FailureCode(terminal->failure->code);
                result.diagnostic = terminal->failure->diagnostic;
                break;
            }
        }
    }

    if (owner_task->file && file_model) {
        file_model->Cancel();
    }
    owner_task.reset();
    owner_task_delivered = true;
    if (!business->DeliverTaskResult(result)) {
        throw std::logic_error("Module rejected its owner task result");
    }
}
} // namespace prism::sdk
