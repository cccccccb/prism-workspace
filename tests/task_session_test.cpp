#include "prism/runtime/task_session.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace {
using prism::runtime::IssueTaskOwnerId;
using prism::runtime::kMaxTaskDiagnosticBytes;
using prism::runtime::TaskCancelReason;
using prism::runtime::TaskEntry;
using prism::runtime::TaskFailure;
using prism::runtime::TaskFailureCode;
using prism::runtime::TaskIdentity;
using prism::runtime::TaskOutcome;
using prism::runtime::TaskOwnerId;
using prism::runtime::TaskPhase;
using prism::runtime::TaskRequestId;
using prism::runtime::TaskSession;
using prism::runtime::TaskTerminal;

TaskIdentity Begin(TaskSession &session)
{
    const auto identity = session.Begin();
    assert(identity);
    assert(identity->owner && identity->request);
    assert(session.Active() == (TaskEntry{*identity, TaskPhase::Preparing}));
    return *identity;
}

TaskTerminal Take(TaskSession &session, TaskIdentity identity, TaskOutcome outcome)
{
    assert(!session.Active());
    const auto terminal = session.TakeTerminal();
    assert(terminal);
    assert(terminal->identity == identity && terminal->outcome == outcome);
    assert(!session.TakeTerminal());
    return *terminal;
}

void CheckStrongIdentity()
{
    static_assert(!std::is_convertible_v<TaskOwnerId, TaskRequestId>);
    static_assert(!std::is_convertible_v<TaskRequestId, TaskOwnerId>);
    static_assert(!std::is_convertible_v<std::uint64_t, TaskOwnerId>);
    static_assert(!std::is_convertible_v<std::uint64_t, TaskRequestId>);
    static_assert(!std::is_copy_constructible_v<TaskSession>);
    static_assert(!std::is_move_constructible_v<TaskSession>);
    static_assert(!std::is_copy_assignable_v<TaskSession>);
    static_assert(!std::is_move_assignable_v<TaskSession>);
    static_assert(!static_cast<bool>(TaskOwnerId{}));
    static_assert(!static_cast<bool>(TaskRequestId{}));
    static_assert(static_cast<bool>(TaskOwnerId{1}));
    static_assert(static_cast<bool>(TaskRequestId{1}));

    bool rejected = false;
    try {
        TaskSession invalid(TaskOwnerId{});
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
}

void CheckPhasesAndPendingTerminal()
{
    const auto owner = IssueTaskOwnerId();
    TaskSession session(owner);
    assert(!session.Active() && !session.TakeTerminal());

    const auto first = Begin(session);
    assert(first.owner == owner && first.request.value == 1);
    assert(!session.Begin());
    assert(!session.SetWorking(first));
    assert(!session.Succeed(first));
    assert(session.Active()->phase == TaskPhase::Preparing);

    assert(session.SetReady(first));
    const auto ready = session.Active();
    assert(!session.SetReady(first) && session.Active() == ready);
    assert(session.SetWorking(first));
    const auto working = session.Active();
    assert(!session.SetWorking(first) && session.Active() == working);

    // A recoverable operation may return to the same ready task.
    assert(session.SetReady(first));
    assert(session.Active()->identity == first);
    assert(session.SetWorking(first));
    assert(session.Succeed(first));
    assert(!session.Begin());
    assert(!session.SetReady(first) && !session.SetWorking(first));
    assert(!session.Succeed(first));
    assert(!session.Cancel(first, TaskCancelReason::User));
    assert(!session.Fail(first, {TaskFailureCode::OperationFailed, "late failure"}));

    const auto success = Take(session, first, TaskOutcome::Success);
    assert(!success.cancel_reason && !success.failure);
    const auto second = Begin(session);
    assert(second.owner == owner && second.request.value == first.request.value + 1);
    assert(session.SetReady(second));
    assert(session.Succeed(second));
    Take(session, second, TaskOutcome::Success);
}

void CheckForeignAndStaleResults()
{
    TaskSession first(IssueTaskOwnerId());
    TaskSession second(IssueTaskOwnerId());
    const auto a = Begin(first);
    const auto b = Begin(second);
    assert(a.owner != b.owner && a.request == b.request);

    const std::array rejected{TaskIdentity{}, TaskIdentity{{}, a.request},
                              TaskIdentity{a.owner, {}}, b,
                              TaskIdentity{a.owner, {a.request.value + 1}}};
    const auto active = first.Active();
    for (const auto identity : rejected) {
        assert(!first.SetReady(identity));
        assert(!first.SetWorking(identity));
        assert(!first.Succeed(identity));
        assert(!first.Cancel(identity, TaskCancelReason::User));
        assert(!first.Fail(identity, {TaskFailureCode::ProviderUnavailable, "foreign result"}));
        assert(first.Active() == active);
        assert(!first.TakeTerminal());
    }

    assert(first.Cancel(a, TaskCancelReason::User));
    Take(first, a, TaskOutcome::Cancelled);
    const auto next = Begin(first);
    assert(next.request.value > a.request.value);
    assert(!first.SetReady(a) && !first.SetWorking(a));
    assert(!first.Succeed(a));
    assert(!first.Cancel(a, TaskCancelReason::Escape));
    assert(!first.Fail(a, {TaskFailureCode::OperationFailed, "stale result"}));
    assert(first.Active()->identity == next);
    assert(second.Active()->identity == b);

    assert(first.Cancel(next, TaskCancelReason::ScopeUnavailable));
    assert(second.SetReady(b) && second.Succeed(b));
    Take(first, next, TaskOutcome::Cancelled);
    Take(second, b, TaskOutcome::Success);
}

void AdvanceTo(TaskSession &session, TaskIdentity identity, TaskPhase phase)
{
    if (phase != TaskPhase::Preparing) {
        assert(session.SetReady(identity));
    }
    if (phase == TaskPhase::Working) {
        assert(session.SetWorking(identity));
    }
}

void CheckCancellationAndFirstTerminal()
{
    const std::array phases{TaskPhase::Preparing, TaskPhase::Ready, TaskPhase::Working};
    const std::array reasons{TaskCancelReason::User,
                             TaskCancelReason::Escape,
                             TaskCancelReason::OwnerClosed,
                             TaskCancelReason::UiReplaced,
                             TaskCancelReason::ScopeUnavailable,
                             TaskCancelReason::FrontendFailed};
    TaskSession session(IssueTaskOwnerId());
    for (const auto phase : phases) {
        for (const auto reason : reasons) {
            const auto identity = Begin(session);
            AdvanceTo(session, identity, phase);
            assert(session.Cancel(identity, reason));
            assert(!session.Cancel(identity, TaskCancelReason::OwnerClosed));
            assert(!session.Succeed(identity));
            assert(!session.Fail(identity, {TaskFailureCode::OperationFailed, "late result"}));
            assert(!session.Begin());

            const auto terminal = Take(session, identity, TaskOutcome::Cancelled);
            assert(terminal.cancel_reason == reason && !terminal.failure);
        }
    }

    const auto identity = Begin(session);
    const auto active = session.Active();
    assert(!session.Cancel(identity, static_cast<TaskCancelReason>(-1)));
    assert(!session.Cancel(identity, static_cast<TaskCancelReason>(128)));
    assert(session.Active() == active && !session.TakeTerminal());
    assert(session.Cancel(identity, TaskCancelReason::User));
    Take(session, identity, TaskOutcome::Cancelled);

    // Cancelling a request does not retire its owner; an explicit retirement does.
    const auto reopened = Begin(session);
    assert(reopened.request.value > identity.request.value);
    session.RetireOwner();
    Take(session, reopened, TaskOutcome::Cancelled);
    assert(!session.Begin());
}

void CheckFailureValidationAndOwnership()
{
    TaskSession session(IssueTaskOwnerId());
    const auto identity = Begin(session);
    const auto active = session.Active();
    const std::array invalid{TaskFailure{static_cast<TaskFailureCode>(-1), "invalid code"},
                             TaskFailure{static_cast<TaskFailureCode>(128), "invalid code"},
                             TaskFailure{TaskFailureCode::OperationFailed,
                                         std::string(kMaxTaskDiagnosticBytes + 1, 'x')}};
    for (const auto &failure : invalid) {
        assert(!session.Fail(identity, failure));
        assert(session.Active() == active);
        assert(!session.TakeTerminal());
        assert(!session.Begin());
    }

    TaskFailure boundary{TaskFailureCode::PreparationFailed,
                         std::string(kMaxTaskDiagnosticBytes, 'x')};
    const auto expected = boundary;
    assert(session.Fail(identity, boundary));
    boundary.diagnostic.clear();
    assert(!session.Cancel(identity, TaskCancelReason::User));
    assert(!session.Succeed(identity));
    assert(!session.Fail(identity, {TaskFailureCode::OperationFailed, "replacement"}));
    assert(!session.Begin());

    const auto terminal = Take(session, identity, TaskOutcome::Failed);
    assert(!terminal.cancel_reason && terminal.failure == expected);

    const std::array codes{TaskFailureCode::PreparationFailed, TaskFailureCode::OperationFailed,
                           TaskFailureCode::ProviderUnavailable};
    const std::array phases{TaskPhase::Preparing, TaskPhase::Ready, TaskPhase::Working};
    for (const auto phase : phases) {
        for (const auto code : codes) {
            const auto next = Begin(session);
            AdvanceTo(session, next, phase);
            const TaskFailure failure{code, {}};
            assert(session.Fail(next, failure));
            const auto result = Take(session, next, TaskOutcome::Failed);
            assert(result.failure == failure && !result.cancel_reason);
        }
    }
}

void CheckOwnerRetirement()
{
    TaskSession never_started(IssueTaskOwnerId());
    never_started.RetireOwner();
    never_started.RetireOwner();
    assert(!never_started.Begin());
    assert(!never_started.Active() && !never_started.TakeTerminal());

    const std::array phases{TaskPhase::Preparing, TaskPhase::Ready, TaskPhase::Working};
    for (const auto phase : phases) {
        TaskSession session(IssueTaskOwnerId());
        const auto identity = Begin(session);
        AdvanceTo(session, identity, phase);
        session.RetireOwner();
        session.RetireOwner();
        assert(!session.Begin());
        assert(!session.SetReady(identity) && !session.SetWorking(identity));
        assert(!session.Succeed(identity));
        assert(!session.Cancel(identity, TaskCancelReason::User));
        assert(!session.Fail(identity, {TaskFailureCode::OperationFailed, "late worker result"}));

        const auto terminal = Take(session, identity, TaskOutcome::Cancelled);
        assert(terminal.cancel_reason == TaskCancelReason::OwnerClosed && !terminal.failure);
        assert(!session.Begin());
        assert(!session.Active() && !session.TakeTerminal());
    }

    // Retirement cannot overwrite an already accepted terminal outcome.
    TaskSession finished(IssueTaskOwnerId());
    const auto identity = Begin(finished);
    assert(finished.SetReady(identity) && finished.Succeed(identity));
    finished.RetireOwner();
    Take(finished, identity, TaskOutcome::Success);
    assert(!finished.Begin());
}

struct ReentrantReceiver {
    TaskSession &session;
    std::optional<TaskIdentity> next;

    void Receive(const TaskTerminal &terminal)
    {
        assert(!session.TakeTerminal());
        next = session.Begin();
        assert(next && next->request.value > terminal.identity.request.value);
        assert(!session.Succeed(terminal.identity));
        assert(!session.Cancel(terminal.identity, TaskCancelReason::User));
        assert(session.Active()->identity == *next);
    }
};

void CheckTakeBeforeCallbackReentry()
{
    TaskSession session(IssueTaskOwnerId());
    const auto first = Begin(session);
    assert(session.SetReady(first) && session.Succeed(first));
    assert(!session.Begin());

    const auto terminal = session.TakeTerminal();
    assert(terminal);
    ReentrantReceiver receiver{session, {}};
    receiver.Receive(*terminal);
    assert(receiver.next);

    // A reentrant close permanently rejects later results for the new task too.
    session.RetireOwner();
    const auto cancelled = Take(session, *receiver.next, TaskOutcome::Cancelled);
    assert(cancelled.cancel_reason == TaskCancelReason::OwnerClosed);
    assert(!session.Succeed(*receiver.next));
    assert(!session.Begin());
}

void CheckReusedStorage()
{
    alignas(TaskSession) std::byte storage[sizeof(TaskSession)];
    auto *first = std::construct_at(reinterpret_cast<TaskSession *>(storage), IssueTaskOwnerId());
    const auto stale = Begin(*first);
    std::destroy_at(first);

    auto *second = std::construct_at(reinterpret_cast<TaskSession *>(storage), IssueTaskOwnerId());
    const auto fresh = Begin(*second);
    assert(fresh.owner.value > stale.owner.value && fresh.request == stale.request);
    assert(!second->SetReady(stale));
    assert(!second->Cancel(stale, TaskCancelReason::User));
    assert(!second->Fail(stale, {TaskFailureCode::ProviderUnavailable, "old provider"}));
    assert(second->Active()->identity == fresh);
    assert(second->Cancel(fresh, TaskCancelReason::User));
    Take(*second, fresh, TaskOutcome::Cancelled);
    std::destroy_at(second);
}

struct AllocateOwners {
    std::span<TaskOwnerId> owners;

    void operator()() const
    {
        TaskOwnerId previous;
        for (auto &owner : owners) {
            owner = IssueTaskOwnerId();
            assert(owner.value > previous.value);
            previous = owner;
        }
    }
};

void CheckMonotonicConcurrentOwners()
{
    const auto lower = IssueTaskOwnerId();
    std::array<std::array<TaskOwnerId, 64>, 4> groups{};
    std::array<std::thread, 4> threads;
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i] = std::thread(AllocateOwners{groups[i]});
    }
    for (auto &thread : threads) {
        thread.join();
    }

    const auto upper = IssueTaskOwnerId();
    std::set<std::uint64_t> unique;
    for (const auto &group : groups) {
        for (const auto owner : group) {
            assert(owner.value > lower.value && owner.value < upper.value);
            assert(unique.insert(owner.value).second);
        }
    }
    assert(unique.size() == groups.size() * groups.front().size());
    assert(upper.value == lower.value + unique.size() + 1);
}

} // namespace

int main()
{
    CheckStrongIdentity();
    CheckPhasesAndPendingTerminal();
    CheckForeignAndStaleResults();
    CheckCancellationAndFirstTerminal();
    CheckFailureValidationAndOwnership();
    CheckOwnerRetirement();
    CheckTakeBeforeCallbackReentry();
    CheckReusedStorage();
    CheckMonotonicConcurrentOwners();
}
