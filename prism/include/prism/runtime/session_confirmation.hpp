#pragma once

#include "prism/contracts/session_confirmation.hpp"

#include <cstdint>
#include <optional>

namespace prism::runtime {

enum class SessionConfirmationPhase { Preparing, Ready };

struct SessionConfirmationEntry {
    contracts::SessionConfirmationIdentity identity;
    contracts::SessionConfirmationPlan plan;
    SessionConfirmationPhase phase{SessionConfirmationPhase::Preparing};
    std::optional<contracts::SessionConfirmationProjection> projection;
    bool operator==(const SessionConfirmationEntry &) const noexcept = default;
};

struct SessionConfirmationTerminal {
    contracts::SessionConfirmationIdentity identity;
    contracts::SessionConfirmationPlan plan;
    contracts::SessionConfirmationOutcome outcome{contracts::SessionConfirmationOutcome::Cancelled};
    std::optional<contracts::SessionConfirmationCancelReason> cancel_reason;
    bool operator==(const SessionConfirmationTerminal &) const noexcept = default;
};

// Serialized owner-thread association core. It owns no transport, authentication,
// input barrier, surface, focus restoration or operation executor. Ordinary
// application requests must never be forwarded here as a privileged entrance.
class SessionConfirmationSession {
public:
    // The future private coordinator allocates a distinct, nonzero lifetime for
    // every real session; the constructor checks nonzero, not its provenance.
    explicit SessionConfirmationSession(std::uint64_t session);
    SessionConfirmationSession(const SessionConfirmationSession &) = delete;
    SessionConfirmationSession &operator=(const SessionConfirmationSession &) = delete;
    SessionConfirmationSession(SessionConfirmationSession &&) = delete;
    SessionConfirmationSession &operator=(SessionConfirmationSession &&) = delete;

    std::optional<contracts::SessionConfirmationIdentity>
    Begin(const contracts::SessionConfirmationPlan &, std::uint64_t now_ns) noexcept;
    std::optional<SessionConfirmationEntry> Active() const noexcept;

    // Bind once before receipt acceptance. Changing a projection requires
    // cancellation and a fresh request, so an old frame cannot confirm a new UI.
    bool BindProjection(contracts::SessionConfirmationIdentity,
                        contracts::SessionConfirmationProjection, std::uint64_t now_ns) noexcept;
    bool AcknowledgeBarrier(const contracts::SessionConfirmationProof &,
                            std::uint64_t now_ns) noexcept;
    bool AdoptPresenterInput(const contracts::SessionConfirmationProof &,
                             std::uint64_t now_ns) noexcept;
    bool Decide(const contracts::SessionConfirmationDecisionProof &, std::uint64_t now_ns) noexcept;
    bool Cancel(contracts::SessionConfirmationIdentity,
                contracts::SessionConfirmationCancelReason) noexcept;
    bool Expire(std::uint64_t now_ns) noexcept;

    // Retirement preserves an already accepted terminal receipt. In particular,
    // ConfirmedIntent remains a receipt, never an execution authorization.
    bool Retire(contracts::SessionConfirmationCancelReason reason =
                    contracts::SessionConfirmationCancelReason::SessionRetired) noexcept;
    // Remove before callbacks; a receiver may begin a fresh request synchronously.
    std::optional<SessionConfirmationTerminal> TakeTerminal() noexcept;

private:
    bool Matches(contracts::SessionConfirmationIdentity) const noexcept;
    bool Matches(const contracts::SessionConfirmationProof &) const noexcept;
    bool ObserveTime(std::uint64_t now_ns) noexcept;
    bool Finish(contracts::SessionConfirmationOutcome,
                std::optional<contracts::SessionConfirmationCancelReason>) noexcept;
    void UpdateReadiness() noexcept;

    const std::uint64_t session_;
    std::uint64_t last_request_{};
    std::uint64_t last_observation_ns_{};
    bool retired_{};
    bool barrier_acknowledged_{};
    bool presenter_adopted_{};
    std::optional<SessionConfirmationEntry> active_;
    std::optional<SessionConfirmationTerminal> terminal_;
};

} // namespace prism::runtime
