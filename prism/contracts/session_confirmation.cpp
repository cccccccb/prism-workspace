#include "prism/contracts/session_confirmation.hpp"

namespace prism::contracts {
namespace {
bool ValidPresenter(SessionConfirmationPresenter presenter) noexcept
{
    return presenter.instance != 0 && presenter.pid != 0;
}

bool ValidScope(SessionConfirmationScope scope) noexcept
{
    return scope.output != 0;
}

bool ValidIntent(SessionConfirmationIntent intent) noexcept
{
    return intent == SessionConfirmationIntent::EndSession;
}

bool ValidDecision(SessionConfirmationDecision decision) noexcept
{
    switch (decision) {
    case SessionConfirmationDecision::Cancel:
    case SessionConfirmationDecision::Confirm:
        return true;
    }
    return false;
}
} // namespace

SessionConfirmationAvailability CurrentSessionConfirmationAvailability() noexcept
{
    return SessionConfirmationAvailability::Unavailable;
}

bool ValidSessionConfirmationIdentity(SessionConfirmationIdentity identity) noexcept
{
    return identity.session != 0 && identity.request != 0;
}

bool ValidSessionConfirmationProjection(SessionConfirmationProjection projection) noexcept
{
    return projection.input_epoch != 0 && projection.ui_generation != 0 &&
           projection.frame_sequence != 0;
}

bool ValidSessionConfirmationCancelReason(SessionConfirmationCancelReason reason) noexcept
{
    switch (reason) {
    case SessionConfirmationCancelReason::User:
    case SessionConfirmationCancelReason::Escape:
    case SessionConfirmationCancelReason::Timeout:
    case SessionConfirmationCancelReason::PresenterUnavailable:
    case SessionConfirmationCancelReason::ProjectionChanged:
    case SessionConfirmationCancelReason::Disconnected:
    case SessionConfirmationCancelReason::SessionRetired:
        return true;
    }
    return false;
}

bool ValidateSessionConfirmationPlan(const SessionConfirmationPlan &plan) noexcept
{
    return ValidIntent(plan.intent) && ValidPresenter(plan.presenter) && ValidScope(plan.scope) &&
           plan.deadline_ns != 0;
}

bool ValidateSessionConfirmationProof(const SessionConfirmationProof &proof) noexcept
{
    return ValidSessionConfirmationIdentity(proof.identity) && ValidPresenter(proof.presenter) &&
           ValidScope(proof.scope) && ValidSessionConfirmationProjection(proof.projection);
}

bool ValidateSessionConfirmationDecisionProof(
    const SessionConfirmationDecisionProof &proof) noexcept
{
    return ValidateSessionConfirmationProof(proof.proof) && ValidDecision(proof.decision) &&
           proof.input_sequence != 0 && proof.protocol_serial != 0;
}

} // namespace prism::contracts
