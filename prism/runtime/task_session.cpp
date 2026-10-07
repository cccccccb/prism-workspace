#include "prism/runtime/task_session.hpp"

#include <atomic>
#include <limits>
#include <stdexcept>
#include <utility>

namespace prism::runtime {
namespace {
bool ValidCancelReason(TaskCancelReason reason) noexcept
{
    switch (reason) {
    case TaskCancelReason::User:
    case TaskCancelReason::Escape:
    case TaskCancelReason::OwnerClosed:
    case TaskCancelReason::UiReplaced:
    case TaskCancelReason::ScopeUnavailable:
    case TaskCancelReason::FrontendFailed:
        return true;
    }
    return false;
}

bool ValidFailure(const TaskFailure &failure) noexcept
{
    if (failure.diagnostic.size() > kMaxTaskDiagnosticBytes) {
        return false;
    }

    switch (failure.code) {
    case TaskFailureCode::PreparationFailed:
    case TaskFailureCode::OperationFailed:
    case TaskFailureCode::ProviderUnavailable:
        return true;
    }
    return false;
}
} // namespace

TaskOwnerId IssueTaskOwnerId()
{
    // This counter establishes uniqueness only; it does not publish session state.
    static std::atomic<std::uint64_t> last_owner{};
    auto previous = last_owner.load(std::memory_order_relaxed);
    for (;;) {
        if (previous == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Task owner IDs exhausted");
        }

        const auto next = previous + 1;
        if (last_owner.compare_exchange_weak(previous, next, std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
            return {next};
        }
    }
}

TaskSession::TaskSession(TaskOwnerId owner) : owner_(owner)
{
    if (!owner_) {
        throw std::invalid_argument("Task owner lifetime must be nonzero");
    }
}

std::optional<TaskIdentity> TaskSession::Begin() noexcept
{
    if (retired_ || active_ || terminal_ ||
        last_request_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }

    const TaskIdentity identity{owner_, {last_request_ + 1}};
    active_ = TaskEntry{identity, TaskPhase::Preparing};
    last_request_ = identity.request.value;
    return identity;
}

std::optional<TaskEntry> TaskSession::Active() const noexcept
{
    return active_;
}

bool TaskSession::Matches(TaskIdentity identity) const noexcept
{
    return !retired_ && active_ && identity.owner && identity.request &&
           active_->identity == identity;
}

bool TaskSession::SetReady(TaskIdentity identity) noexcept
{
    if (!Matches(identity) || active_->phase == TaskPhase::Ready) {
        return false;
    }

    active_->phase = TaskPhase::Ready;
    return true;
}

bool TaskSession::SetWorking(TaskIdentity identity) noexcept
{
    if (!Matches(identity) || active_->phase != TaskPhase::Ready) {
        return false;
    }

    active_->phase = TaskPhase::Working;
    return true;
}

bool TaskSession::Reprepare(TaskIdentity identity) noexcept
{
    if (!Matches(identity)) {
        return false;
    }

    active_->phase = TaskPhase::Preparing;
    return true;
}

bool TaskSession::Finish(TaskTerminal terminal) noexcept
{
    if (!Matches(terminal.identity) || terminal_) {
        return false;
    }

    terminal_ = std::move(terminal);
    active_.reset();
    return true;
}

bool TaskSession::Succeed(TaskIdentity identity) noexcept
{
    if (!Matches(identity) || active_->phase == TaskPhase::Preparing) {
        return false;
    }

    return Finish({identity, TaskOutcome::Success, std::nullopt, std::nullopt});
}

bool TaskSession::Cancel(TaskIdentity identity, TaskCancelReason reason) noexcept
{
    if (!Matches(identity) || !ValidCancelReason(reason)) {
        return false;
    }

    return Finish({identity, TaskOutcome::Cancelled, reason, std::nullopt});
}

bool TaskSession::Fail(TaskIdentity identity, const TaskFailure &failure)
{
    if (!Matches(identity) || !ValidFailure(failure)) {
        return false;
    }

    // Copying the diagnostic may allocate. Prepare it before changing the active
    // request, so an allocation failure also preserves the current session.
    TaskTerminal terminal{identity, TaskOutcome::Failed, std::nullopt, failure};

    return Finish(std::move(terminal));
}

void TaskSession::RetireOwner() noexcept
{
    if (retired_) {
        return;
    }

    if (active_) {
        Cancel(active_->identity, TaskCancelReason::OwnerClosed);
    }
    retired_ = true;
}

std::optional<TaskTerminal> TaskSession::TakeTerminal() noexcept
{
    return std::exchange(terminal_, std::nullopt);
}

} // namespace prism::runtime
