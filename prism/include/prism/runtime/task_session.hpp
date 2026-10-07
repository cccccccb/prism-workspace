#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace prism::runtime {

// Process-local lifetime identities, not window indices or transport authorization.
struct TaskOwnerId {
    std::uint64_t value{};

    constexpr explicit operator bool() const noexcept
    {
        return value != 0;
    }

    constexpr bool operator==(const TaskOwnerId &) const noexcept = default;
};

struct TaskRequestId {
    std::uint64_t value{};

    constexpr explicit operator bool() const noexcept
    {
        return value != 0;
    }

    constexpr bool operator==(const TaskRequestId &) const noexcept = default;
};

struct TaskIdentity {
    TaskOwnerId owner;
    TaskRequestId request;
    constexpr bool operator==(const TaskIdentity &) const noexcept = default;
};

// Independent owners may allocate concurrently. Exhaustion throws overflow_error;
// IDs never wrap or reuse. Call only in the final runtime, not a pre-fork seed.
TaskOwnerId IssueTaskOwnerId();

enum class TaskPhase { Preparing, Ready, Working };
enum class TaskOutcome { Success, Cancelled, Failed };
enum class TaskCancelReason {
    User,
    Escape,
    OwnerClosed,
    UiReplaced,
    ScopeUnavailable,
    FrontendFailed
};
enum class TaskFailureCode { PreparationFailed, OperationFailed, ProviderUnavailable };

inline constexpr std::size_t kMaxTaskDiagnosticBytes = 1024;

struct TaskFailure {
    TaskFailureCode code{TaskFailureCode::OperationFailed};
    // Optional descriptive text; typed code determines the failure category.
    std::string diagnostic;
    bool operator==(const TaskFailure &) const = default;
};

struct TaskEntry {
    TaskIdentity identity;
    TaskPhase phase{TaskPhase::Preparing};
    bool operator==(const TaskEntry &) const noexcept = default;
};

struct TaskTerminal {
    TaskIdentity identity;
    TaskOutcome outcome{TaskOutcome::Success};
    // Present only for the matching outcome. No service-specific result payload.
    std::optional<TaskCancelReason> cancel_reason;
    std::optional<TaskFailure> failure;
    bool operator==(const TaskTerminal &) const = default;
};

// Owner-thread policy core, independent of Scene, modal presentation and callbacks.
// There is at most one active request or one pending terminal. Remove the terminal
// before invoking business code; the callback may synchronously start another task.
class TaskSession {
public:
    explicit TaskSession(TaskOwnerId owner);
    TaskSession(const TaskSession &) = delete;
    TaskSession &operator=(const TaskSession &) = delete;
    TaskSession(TaskSession &&) = delete;
    TaskSession &operator=(TaskSession &&) = delete;

    // Busy, retired or exhausted sessions reject without changing state.
    std::optional<TaskIdentity> Begin() noexcept;
    std::optional<TaskEntry> Active() const noexcept;
    bool SetReady(TaskIdentity identity) noexcept;
    // A new semantic projection must adopt fresh input before becoming Ready.
    bool Reprepare(TaskIdentity identity) noexcept;
    bool SetWorking(TaskIdentity identity) noexcept;
    bool Succeed(TaskIdentity identity) noexcept;
    bool Cancel(TaskIdentity identity, TaskCancelReason reason) noexcept;
    // Invalid code or diagnostic larger than the byte limit rejects atomically.
    bool Fail(TaskIdentity identity, const TaskFailure &failure);
    // Permanent retirement cancels an active request with OwnerClosed. A terminal
    // already accepted is preserved; the Host decides whether delivery is still valid.
    void RetireOwner() noexcept;
    std::optional<TaskTerminal> TakeTerminal() noexcept;

private:
    bool Matches(TaskIdentity identity) const noexcept;
    bool Finish(TaskTerminal terminal) noexcept;

    const TaskOwnerId owner_;
    std::uint64_t last_request_{};
    bool retired_{};
    std::optional<TaskEntry> active_;
    std::optional<TaskTerminal> terminal_;
};

} // namespace prism::runtime
