#include "prism/runtime/session_confirmation.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace prism::runtime {
using contracts::SessionConfirmationCancelReason;
using contracts::SessionConfirmationDecision;
using contracts::SessionConfirmationDecisionProof;
using contracts::SessionConfirmationIdentity;
using contracts::SessionConfirmationOutcome;
using contracts::SessionConfirmationPlan;
using contracts::SessionConfirmationProjection;
using contracts::SessionConfirmationProof;

SessionConfirmationSession::SessionConfirmationSession(std::uint64_t session) : session_(session)
{
    if (session_ == 0) {
        throw std::invalid_argument("Session confirmation lifetime must be nonzero");
    }
}

std::optional<SessionConfirmationIdentity>
SessionConfirmationSession::Begin(const SessionConfirmationPlan &plan,
                                  std::uint64_t now_ns) noexcept
{
    if (retired_ || active_ || terminal_ || !contracts::ValidateSessionConfirmationPlan(plan) ||
        plan.deadline_ns <= now_ns || now_ns < last_observation_ns_ ||
        last_request_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }

    const SessionConfirmationIdentity identity{session_, last_request_ + 1};
    active_ = SessionConfirmationEntry{identity, plan, SessionConfirmationPhase::Preparing, {}};
    last_request_ = identity.request;
    last_observation_ns_ = now_ns;
    barrier_acknowledged_ = false;
    presenter_adopted_ = false;
    return identity;
}

std::optional<SessionConfirmationEntry> SessionConfirmationSession::Active() const noexcept
{
    return active_;
}

bool SessionConfirmationSession::Matches(SessionConfirmationIdentity identity) const noexcept
{
    return !retired_ && active_ && contracts::ValidSessionConfirmationIdentity(identity) &&
           active_->identity == identity;
}

bool SessionConfirmationSession::Matches(const SessionConfirmationProof &proof) const noexcept
{
    return contracts::ValidateSessionConfirmationProof(proof) && Matches(proof.identity) &&
           proof.presenter == active_->plan.presenter && proof.scope == active_->plan.scope &&
           active_->projection == proof.projection;
}

bool SessionConfirmationSession::ObserveTime(std::uint64_t now_ns) noexcept
{
    if (retired_ || !active_ || now_ns < last_observation_ns_) {
        return false;
    }

    last_observation_ns_ = now_ns;
    if (now_ns >= active_->plan.deadline_ns) {
        Finish(SessionConfirmationOutcome::Cancelled, SessionConfirmationCancelReason::Timeout);
        return false;
    }
    return true;
}

bool SessionConfirmationSession::BindProjection(SessionConfirmationIdentity identity,
                                                SessionConfirmationProjection projection,
                                                std::uint64_t now_ns) noexcept
{
    if (!Matches(identity) || active_->projection ||
        !contracts::ValidSessionConfirmationProjection(projection) || !ObserveTime(now_ns)) {
        return false;
    }

    active_->projection = projection;
    return true;
}

void SessionConfirmationSession::UpdateReadiness() noexcept
{
    if (barrier_acknowledged_ && presenter_adopted_) {
        active_->phase = SessionConfirmationPhase::Ready;
    }
}

bool SessionConfirmationSession::AcknowledgeBarrier(const SessionConfirmationProof &proof,
                                                    std::uint64_t now_ns) noexcept
{
    if (!Matches(proof) || barrier_acknowledged_ || !ObserveTime(now_ns)) {
        return false;
    }

    barrier_acknowledged_ = true;
    UpdateReadiness();
    return true;
}

bool SessionConfirmationSession::AdoptPresenterInput(const SessionConfirmationProof &proof,
                                                     std::uint64_t now_ns) noexcept
{
    if (!Matches(proof) || presenter_adopted_ || !ObserveTime(now_ns)) {
        return false;
    }

    presenter_adopted_ = true;
    UpdateReadiness();
    return true;
}

bool SessionConfirmationSession::Finish(
    SessionConfirmationOutcome outcome,
    std::optional<SessionConfirmationCancelReason> cancel_reason) noexcept
{
    if (retired_ || !active_ || terminal_) {
        return false;
    }

    terminal_ =
        SessionConfirmationTerminal{active_->identity, active_->plan, outcome, cancel_reason};
    active_.reset();
    barrier_acknowledged_ = false;
    presenter_adopted_ = false;
    return true;
}

bool SessionConfirmationSession::Decide(const SessionConfirmationDecisionProof &proof,
                                        std::uint64_t now_ns) noexcept
{
    if (!contracts::ValidateSessionConfirmationDecisionProof(proof) || !Matches(proof.proof) ||
        active_->phase != SessionConfirmationPhase::Ready || !ObserveTime(now_ns)) {
        return false;
    }

    if (proof.decision == SessionConfirmationDecision::Cancel) {
        return Finish(SessionConfirmationOutcome::Cancelled, SessionConfirmationCancelReason::User);
    }
    return Finish(SessionConfirmationOutcome::ConfirmedIntent, std::nullopt);
}

bool SessionConfirmationSession::Cancel(SessionConfirmationIdentity identity,
                                        SessionConfirmationCancelReason reason) noexcept
{
    if (!Matches(identity) || !contracts::ValidSessionConfirmationCancelReason(reason)) {
        return false;
    }

    return Finish(SessionConfirmationOutcome::Cancelled, reason);
}

bool SessionConfirmationSession::Expire(std::uint64_t now_ns) noexcept
{
    if (retired_ || !active_ || now_ns < last_observation_ns_) {
        return false;
    }

    last_observation_ns_ = now_ns;
    if (now_ns < active_->plan.deadline_ns) {
        return false;
    }

    return Finish(SessionConfirmationOutcome::Cancelled, SessionConfirmationCancelReason::Timeout);
}

bool SessionConfirmationSession::Retire(SessionConfirmationCancelReason reason) noexcept
{
    if (retired_ || !contracts::ValidSessionConfirmationCancelReason(reason)) {
        return false;
    }

    if (active_) {
        Cancel(active_->identity, reason);
    }
    retired_ = true;
    return true;
}

std::optional<SessionConfirmationTerminal> SessionConfirmationSession::TakeTerminal() noexcept
{
    return std::exchange(terminal_, std::nullopt);
}

} // namespace prism::runtime
