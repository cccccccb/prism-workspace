#include "prism/contracts/session_confirmation.hpp"
#include "prism/runtime/session_confirmation.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace {
using namespace prism::contracts;
using prism::runtime::SessionConfirmationEntry;
using prism::runtime::SessionConfirmationPhase;
using prism::runtime::SessionConfirmationSession;
using prism::runtime::SessionConfirmationTerminal;

constexpr SessionConfirmationProjection kProjection{13, 17, 19};
constexpr std::uint64_t kStartNs = 10;
constexpr std::uint64_t kDeadlineNs = 100;

SessionConfirmationPlan Plan(std::uint64_t deadline_ns = kDeadlineNs)
{
    return {SessionConfirmationIntent::EndSession, {23, 29}, {31, 0}, deadline_ns};
}

SessionConfirmationIdentity Begin(SessionConfirmationSession &session,
                                  std::uint64_t now_ns = kStartNs,
                                  std::uint64_t deadline_ns = kDeadlineNs)
{
    const auto plan = Plan(deadline_ns);
    const auto identity = session.Begin(plan, now_ns);
    assert(identity && identity->session != 0 && identity->request != 0);
    assert(session.Active() ==
           (SessionConfirmationEntry{*identity, plan, SessionConfirmationPhase::Preparing, {}}));
    return *identity;
}

SessionConfirmationProof Proof(SessionConfirmationIdentity identity,
                               SessionConfirmationProjection projection = kProjection)
{
    const auto plan = Plan();
    return {identity, plan.presenter, plan.scope, projection};
}

SessionConfirmationDecisionProof
Decision(SessionConfirmationIdentity identity,
         SessionConfirmationDecision decision = SessionConfirmationDecision::Confirm)
{
    return {Proof(identity), decision, 37, 41};
}

void MakeReady(SessionConfirmationSession &session, SessionConfirmationIdentity identity,
               std::uint64_t now_ns = kStartNs)
{
    assert(session.BindProjection(identity, kProjection, now_ns));
    assert(session.AcknowledgeBarrier(Proof(identity), now_ns));
    assert(session.Active()->phase == SessionConfirmationPhase::Preparing);
    assert(session.AdoptPresenterInput(Proof(identity), now_ns));
    assert(session.Active()->phase == SessionConfirmationPhase::Ready);
}

SessionConfirmationTerminal Take(SessionConfirmationSession &session,
                                 SessionConfirmationIdentity identity,
                                 SessionConfirmationOutcome outcome)
{
    assert(!session.Active());
    const auto terminal = session.TakeTerminal();
    assert(terminal && terminal->identity == identity && terminal->outcome == outcome);
    assert(terminal->plan.intent == SessionConfirmationIntent::EndSession);
    assert(!session.TakeTerminal());
    return *terminal;
}

void CheckUnavailableAndConstruction()
{
    static_assert(!std::is_copy_constructible_v<SessionConfirmationSession>);
    static_assert(!std::is_copy_assignable_v<SessionConfirmationSession>);
    static_assert(!std::is_move_constructible_v<SessionConfirmationSession>);
    static_assert(!std::is_move_assignable_v<SessionConfirmationSession>);
    assert(CurrentSessionConfirmationAvailability() ==
           SessionConfirmationAvailability::Unavailable);

    bool rejected = false;
    try {
        SessionConfirmationSession invalid(0);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);

    SessionConfirmationSession session(43);
    const auto identity = Begin(session);
    MakeReady(session, identity);
    // Association readiness cannot enable a missing platform capability.
    assert(CurrentSessionConfirmationAvailability() ==
           SessionConfirmationAvailability::Unavailable);
    assert(session.Decide(Decision(identity), kStartNs));
    const auto terminal = Take(session, identity, SessionConfirmationOutcome::ConfirmedIntent);
    assert(!terminal.cancel_reason);
    assert(CurrentSessionConfirmationAvailability() ==
           SessionConfirmationAvailability::Unavailable);
}

void CheckContractValidation()
{
    assert(ValidateSessionConfirmationPlan(Plan()));
    auto plan = Plan();
    plan.intent = static_cast<SessionConfirmationIntent>(0);
    assert(!ValidateSessionConfirmationPlan(plan));
    plan = Plan();
    plan.presenter.instance = 0;
    assert(!ValidateSessionConfirmationPlan(plan));
    plan = Plan();
    plan.presenter.pid = 0;
    assert(!ValidateSessionConfirmationPlan(plan));
    plan = Plan();
    plan.scope.output = 0;
    assert(!ValidateSessionConfirmationPlan(plan));
    plan = Plan(0);
    assert(!ValidateSessionConfirmationPlan(plan));

    const SessionConfirmationIdentity identity{43, 1};
    auto proof = Proof(identity);
    assert(ValidateSessionConfirmationProof(proof));
    proof.identity.session = 0;
    assert(!ValidateSessionConfirmationProof(proof));
    proof = Proof(identity);
    proof.identity.request = 0;
    assert(!ValidateSessionConfirmationProof(proof));
    proof = Proof(identity);
    proof.presenter.instance = 0;
    assert(!ValidateSessionConfirmationProof(proof));
    proof = Proof(identity);
    proof.presenter.pid = 0;
    assert(!ValidateSessionConfirmationProof(proof));
    proof = Proof(identity);
    proof.scope.output = 0;
    assert(!ValidateSessionConfirmationProof(proof));
    const std::array invalid_projections{SessionConfirmationProjection{0, 17, 19},
                                         SessionConfirmationProjection{13, 0, 19},
                                         SessionConfirmationProjection{13, 17, 0}};
    for (const auto projection : invalid_projections) {
        proof = Proof(identity, projection);
        assert(!ValidSessionConfirmationProjection(projection));
        assert(!ValidateSessionConfirmationProof(proof));
    }

    auto decision = Decision(identity);
    assert(ValidateSessionConfirmationDecisionProof(decision));
    decision.decision = static_cast<SessionConfirmationDecision>(99);
    assert(!ValidateSessionConfirmationDecisionProof(decision));
    decision = Decision(identity);
    decision.input_sequence = 0;
    assert(!ValidateSessionConfirmationDecisionProof(decision));
    decision = Decision(identity);
    decision.protocol_serial = 0;
    assert(!ValidateSessionConfirmationDecisionProof(decision));
}

void CheckBeginAndProjection()
{
    SessionConfirmationSession session(43);
    assert(!session.Begin(Plan(), kDeadlineNs));
    assert(!session.Begin(Plan(), kDeadlineNs + 1));
    assert(!session.Begin(Plan(0), 0));
    assert(!session.Active() && !session.TakeTerminal());

    auto invalid = Plan();
    invalid.presenter.instance = 0;
    assert(!session.Begin(invalid, kStartNs));
    const auto identity = Begin(session);
    assert(identity.request == 1);
    assert(!session.Begin(Plan(), kStartNs));
    assert(!session.Decide(Decision(identity), kStartNs));
    assert(!session.AcknowledgeBarrier(Proof(identity), kStartNs));
    assert(!session.AdoptPresenterInput(Proof(identity), kStartNs));
    assert(session.Active()->phase == SessionConfirmationPhase::Preparing);
    assert(!session.Active()->projection);

    assert(!session.BindProjection(identity, {}, kStartNs));
    assert(!session.BindProjection({43, 2}, kProjection, kStartNs));
    assert(session.BindProjection(identity, kProjection, kStartNs));
    const auto bound = session.Active();
    assert(!session.BindProjection(identity, kProjection, kStartNs));
    assert(!session.BindProjection(identity, {13, 17, 20}, kStartNs));
    assert(session.Active() == bound);

    assert(session.AdoptPresenterInput(Proof(identity), kStartNs));
    const auto adopted = session.Active();
    assert(!session.AdoptPresenterInput(Proof(identity), kStartNs));
    assert(session.Active() == adopted);
    assert(!session.Decide(Decision(identity), kStartNs));
    assert(session.AcknowledgeBarrier(Proof(identity), kStartNs));
    assert(session.Active()->phase == SessionConfirmationPhase::Ready);
    const auto ready = session.Active();
    assert(!session.AcknowledgeBarrier(Proof(identity), kStartNs));
    assert(!session.AdoptPresenterInput(Proof(identity), kStartNs));
    assert(session.Active() == ready);

    assert(session.Decide(Decision(identity), kStartNs));
    assert(!session.Decide(Decision(identity), kStartNs));
    assert(!session.Cancel(identity, SessionConfirmationCancelReason::User));
    assert(!session.Begin(Plan(), kStartNs));
    Take(session, identity, SessionConfirmationOutcome::ConfirmedIntent);
}

std::array<SessionConfirmationProof, 10> MismatchedProofs(SessionConfirmationIdentity identity)
{
    std::array<SessionConfirmationProof, 10> proofs;
    proofs.fill(Proof(identity));
    proofs[0].identity.session = 0;
    proofs[1].identity.session += 1;
    proofs[2].identity.request = 0;
    proofs[3].identity.request += 1;
    proofs[4].presenter.instance += 1;
    proofs[5].presenter.pid += 1;
    proofs[6].scope.output += 1;
    proofs[7].scope.seat += 1;
    proofs[8].projection.input_epoch += 1;
    proofs[9].projection.frame_sequence += 1;
    return proofs;
}

void CheckWrongAndStaleProofs()
{
    SessionConfirmationSession session(43);
    const auto identity = Begin(session);
    assert(session.BindProjection(identity, kProjection, kStartNs));
    const auto bound = session.Active();
    for (const auto &proof : MismatchedProofs(identity)) {
        assert(!session.AcknowledgeBarrier(proof, kStartNs));
        assert(!session.AdoptPresenterInput(proof, kStartNs));
        assert(!session.Decide({proof, SessionConfirmationDecision::Confirm, 37, 41}, kStartNs));
        assert(session.Active() == bound && !session.TakeTerminal());
    }

    auto changed_ui = Proof(identity);
    changed_ui.projection.ui_generation += 1;
    assert(!session.AcknowledgeBarrier(changed_ui, kStartNs));
    assert(!session.AdoptPresenterInput(changed_ui, kStartNs));
    assert(session.AcknowledgeBarrier(Proof(identity), kStartNs));
    assert(session.AdoptPresenterInput(Proof(identity), kStartNs));
    const auto ready = session.Active();
    for (const auto &proof : MismatchedProofs(identity)) {
        assert(!session.Decide({proof, SessionConfirmationDecision::Confirm, 37, 41}, kStartNs));
        assert(session.Active() == ready);
    }
    assert(!session.Decide({changed_ui, SessionConfirmationDecision::Confirm, 37, 41}, kStartNs));

    assert(session.Cancel(identity, SessionConfirmationCancelReason::ProjectionChanged));
    Take(session, identity, SessionConfirmationOutcome::Cancelled);
    const auto successor = Begin(session);
    assert(successor.request > identity.request && successor.session == identity.session);
    MakeReady(session, successor);
    assert(!session.AcknowledgeBarrier(Proof(identity), kStartNs));
    assert(!session.AdoptPresenterInput(Proof(identity), kStartNs));
    assert(!session.Decide(Decision(identity), kStartNs));
    assert(!session.Cancel(identity, SessionConfirmationCancelReason::User));
    assert(session.Active()->identity == successor);

    // A new real session lifetime rejects the old session even if its request
    // number, presenter and projection happen to be reused.
    SessionConfirmationSession replacement(44);
    const auto replacement_identity = Begin(replacement);
    MakeReady(replacement, replacement_identity);
    assert(replacement_identity.request == identity.request);
    assert(!replacement.Decide(Decision(identity), kStartNs));
    assert(replacement.Active()->identity == replacement_identity);
}

void CheckInvalidDecisionAndCancel()
{
    SessionConfirmationSession session(43);
    const auto identity = Begin(session);
    MakeReady(session, identity);
    const auto ready = session.Active();

    auto decision = Decision(identity);
    decision.input_sequence = 0;
    assert(!session.Decide(decision, kStartNs));
    decision = Decision(identity);
    decision.protocol_serial = 0;
    assert(!session.Decide(decision, kStartNs));
    decision = Decision(identity);
    decision.decision = static_cast<SessionConfirmationDecision>(2);
    assert(!session.Decide(decision, kStartNs));
    assert(!session.Cancel(identity, static_cast<SessionConfirmationCancelReason>(0)));
    assert(!session.Retire(static_cast<SessionConfirmationCancelReason>(99)));
    assert(session.Active() == ready && !session.TakeTerminal());

    assert(session.Decide(Decision(identity, SessionConfirmationDecision::Cancel), kStartNs));
    const auto terminal = Take(session, identity, SessionConfirmationOutcome::Cancelled);
    assert(terminal.cancel_reason == SessionConfirmationCancelReason::User);
}

void CheckCancellationInEveryPhase()
{
    const std::array reasons{SessionConfirmationCancelReason::User,
                             SessionConfirmationCancelReason::Escape,
                             SessionConfirmationCancelReason::Timeout,
                             SessionConfirmationCancelReason::PresenterUnavailable,
                             SessionConfirmationCancelReason::ProjectionChanged,
                             SessionConfirmationCancelReason::Disconnected,
                             SessionConfirmationCancelReason::SessionRetired};
    for (const auto phase :
         {SessionConfirmationPhase::Preparing, SessionConfirmationPhase::Ready}) {
        SessionConfirmationSession session(43);
        for (const auto reason : reasons) {
            const auto identity = Begin(session);
            if (phase == SessionConfirmationPhase::Ready) {
                MakeReady(session, identity);
            }
            assert(session.Cancel(identity, reason));
            assert(!session.Cancel(identity, reason));
            assert(!session.Decide(Decision(identity), kStartNs));
            assert(!session.Begin(Plan(), kStartNs));
            const auto terminal = Take(session, identity, SessionConfirmationOutcome::Cancelled);
            assert(terminal.cancel_reason == reason);
        }
    }
}

void CheckTimeoutAndClockScope()
{
    for (unsigned stage = 0; stage != 4; ++stage) {
        SessionConfirmationSession session(43);
        const auto identity = Begin(session);
        if (stage != 0) {
            assert(session.BindProjection(identity, kProjection, kStartNs));
        }
        if (stage >= 2) {
            assert(session.AcknowledgeBarrier(Proof(identity), kStartNs));
        }
        if (stage == 3) {
            assert(session.AdoptPresenterInput(Proof(identity), kStartNs));
        }

        assert(!session.Expire(kDeadlineNs - 1));
        assert(session.Active());
        assert(session.Expire(kDeadlineNs));
        assert(!session.Expire(kDeadlineNs + 1));
        assert(!session.Decide(Decision(identity), kDeadlineNs));
        const auto terminal = Take(session, identity, SessionConfirmationOutcome::Cancelled);
        assert(terminal.cancel_reason == SessionConfirmationCancelReason::Timeout);
        assert(!session.Begin(Plan(200), kStartNs));
        const auto next = Begin(session, kDeadlineNs, 200);
        assert(next.request > identity.request);
    }

    SessionConfirmationSession expired_before_binding(43);
    const auto first = Begin(expired_before_binding);
    assert(!expired_before_binding.BindProjection(first, kProjection, kDeadlineNs));
    assert(
        Take(expired_before_binding, first, SessionConfirmationOutcome::Cancelled).cancel_reason ==
        SessionConfirmationCancelReason::Timeout);

    SessionConfirmationSession expired_before_adoption(43);
    const auto second = Begin(expired_before_adoption);
    assert(expired_before_adoption.BindProjection(second, kProjection, kStartNs));
    assert(expired_before_adoption.AcknowledgeBarrier(Proof(second), kStartNs));
    assert(!expired_before_adoption.AdoptPresenterInput(Proof(second), kDeadlineNs));
    assert(Take(expired_before_adoption, second, SessionConfirmationOutcome::Cancelled)
               .cancel_reason == SessionConfirmationCancelReason::Timeout);

    SessionConfirmationSession expired_before_decision(43);
    const auto third = Begin(expired_before_decision);
    MakeReady(expired_before_decision, third);
    assert(!expired_before_decision.Decide(Decision(third), kDeadlineNs));
    assert(
        Take(expired_before_decision, third, SessionConfirmationOutcome::Cancelled).cancel_reason ==
        SessionConfirmationCancelReason::Timeout);

    SessionConfirmationSession clock(43);
    const auto identity = Begin(clock);
    assert(!clock.BindProjection(identity, kProjection, kStartNs - 1));
    assert(clock.BindProjection(identity, kProjection, kStartNs + 2));
    const auto bound = clock.Active();
    assert(!clock.AcknowledgeBarrier(Proof(identity), kStartNs + 1));
    assert(!clock.AdoptPresenterInput(Proof(identity), kStartNs + 1));
    assert(!clock.Expire(kStartNs + 1));
    assert(clock.Active() == bound);
    assert(clock.AcknowledgeBarrier(Proof(identity), kStartNs + 2));
    assert(clock.AdoptPresenterInput(Proof(identity), kStartNs + 2));
    assert(!clock.Decide(Decision(identity), kStartNs + 1));
    assert(clock.Decide(Decision(identity), kDeadlineNs - 1));
    Take(clock, identity, SessionConfirmationOutcome::ConfirmedIntent);

    // Absolute deadlines require no addition, so the upper integer boundary
    // cannot wrap into an immediate or extended timeout.
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    SessionConfirmationSession upper(43);
    const auto upper_identity = Begin(upper, maximum - 1, maximum);
    assert(upper.Expire(maximum));
    Take(upper, upper_identity, SessionConfirmationOutcome::Cancelled);
}

void CheckRetirementAndPendingReceipt()
{
    SessionConfirmationSession empty(43);
    assert(empty.Retire());
    assert(!empty.Retire());
    assert(!empty.Begin(Plan(), kStartNs));
    assert(!empty.Active() && !empty.TakeTerminal());

    for (const auto phase :
         {SessionConfirmationPhase::Preparing, SessionConfirmationPhase::Ready}) {
        SessionConfirmationSession session(43);
        const auto identity = Begin(session);
        if (phase == SessionConfirmationPhase::Ready) {
            MakeReady(session, identity);
        }
        assert(session.Retire(SessionConfirmationCancelReason::Disconnected));
        assert(!session.Retire());
        assert(!session.AcknowledgeBarrier(Proof(identity), kStartNs));
        assert(!session.AdoptPresenterInput(Proof(identity), kStartNs));
        assert(!session.Decide(Decision(identity), kStartNs));
        assert(!session.Cancel(identity, SessionConfirmationCancelReason::User));
        assert(!session.Expire(kDeadlineNs));
        const auto terminal = Take(session, identity, SessionConfirmationOutcome::Cancelled);
        assert(terminal.cancel_reason == SessionConfirmationCancelReason::Disconnected);
        assert(!session.Begin(Plan(), kStartNs));
    }

    SessionConfirmationSession confirmed(43);
    const auto identity = Begin(confirmed);
    MakeReady(confirmed, identity);
    assert(confirmed.Decide(Decision(identity), kStartNs));
    assert(confirmed.Retire());
    const auto terminal = Take(confirmed, identity, SessionConfirmationOutcome::ConfirmedIntent);
    assert(!terminal.cancel_reason);
    assert(!confirmed.Begin(Plan(), kStartNs));
}

struct ReentrantReceiver {
    SessionConfirmationSession &session;
    std::optional<SessionConfirmationIdentity> next;

    void Receive(const SessionConfirmationTerminal &terminal)
    {
        assert(!session.TakeTerminal());
        next = session.Begin(Plan(), kStartNs);
        assert(next && next->request > terminal.identity.request);
        assert(!session.Decide(Decision(terminal.identity), kStartNs));
        assert(!session.Cancel(terminal.identity, SessionConfirmationCancelReason::User));
        assert(session.Active()->identity == *next);
    }
};

void CheckTakeBeforeReentryAndExhaustion()
{
    SessionConfirmationSession session(43);
    const auto identity = Begin(session);
    assert(session.Cancel(identity, SessionConfirmationCancelReason::Escape));
    assert(!session.Begin(Plan(), kStartNs));
    const auto terminal = session.TakeTerminal();
    assert(terminal);
    ReentrantReceiver receiver{session, {}};
    receiver.Receive(*terminal);
    assert(receiver.next);

    // Test-only access is enabled for this target; production exposes no ID
    // mutation hook or lower-security constructor.
    SessionConfirmationSession exhausted(43);
    exhausted.last_request_ = std::numeric_limits<std::uint64_t>::max() - 1;
    const auto final_identity = Begin(exhausted);
    assert(final_identity.request == std::numeric_limits<std::uint64_t>::max());
    assert(exhausted.Cancel(final_identity, SessionConfirmationCancelReason::User));
    Take(exhausted, final_identity, SessionConfirmationOutcome::Cancelled);
    assert(!exhausted.Begin(Plan(), kStartNs));
    assert(!exhausted.Active() && !exhausted.TakeTerminal());
}

} // namespace

int main()
{
    CheckUnavailableAndConstruction();
    CheckContractValidation();
    CheckBeginAndProjection();
    CheckWrongAndStaleProofs();
    CheckInvalidDecisionAndCancel();
    CheckCancellationInEveryPhase();
    CheckTimeoutAndClockScope();
    CheckRetirementAndPendingReceipt();
    CheckTakeBeforeReentryAndExhaustion();
}
