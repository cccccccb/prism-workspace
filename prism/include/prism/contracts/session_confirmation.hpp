#pragma once

#include <cstdint>

namespace prism::contracts {

// Internal association data only. Matching these values does not authenticate a
// peer, grant a desktop input barrier or authorize termination of the session.
struct SessionConfirmationIdentity {
    std::uint64_t session{};
    std::uint64_t request{};
    bool operator==(const SessionConfirmationIdentity &) const noexcept = default;
};

struct SessionConfirmationPresenter {
    std::uint64_t instance{};
    std::uint32_t pid{};
    bool operator==(const SessionConfirmationPresenter &) const noexcept = default;
};

struct SessionConfirmationScope {
    std::uint64_t output{};
    // Zero is the existing compositor seat; it is a valid association value.
    std::uint32_t seat{};
    bool operator==(const SessionConfirmationScope &) const noexcept = default;
};

struct SessionConfirmationProjection {
    std::uint64_t input_epoch{};
    std::uint64_t ui_generation{};
    std::uint64_t frame_sequence{};
    bool operator==(const SessionConfirmationProjection &) const noexcept = default;
};

enum class SessionConfirmationIntent : std::uint32_t { EndSession = 1 };
enum class SessionConfirmationDecision : std::uint32_t { Cancel = 0, Confirm = 1 };
enum class SessionConfirmationOutcome : std::uint32_t { Cancelled = 0, ConfirmedIntent = 1 };
enum class SessionConfirmationCancelReason : std::uint32_t {
    User = 1,
    Escape = 2,
    Timeout = 3,
    PresenterUnavailable = 4,
    ProjectionChanged = 5,
    Disconnected = 6,
    SessionRetired = 7
};

// The platform admission, presenter and WM barrier are not connected yet. No
// "available" value is exposed until that production integration exists.
enum class SessionConfirmationAvailability : std::uint32_t { Unavailable = 0 };
SessionConfirmationAvailability CurrentSessionConfirmationAvailability() noexcept;

struct SessionConfirmationPlan {
    SessionConfirmationIntent intent{SessionConfirmationIntent::EndSession};
    SessionConfirmationPresenter presenter;
    SessionConfirmationScope scope;
    // Absolute monotonic deadline, selected by the internal coordinator. It
    // covers Preparing and Ready; no wall clock or module-supplied timestamp.
    std::uint64_t deadline_ns{};
    bool operator==(const SessionConfirmationPlan &) const noexcept = default;
};

// A future platform adapter constructs receipts only after checking the actual
// private endpoints and associated live surfaces. This DTO itself is no proof
// of their authenticity; the core checks equality and lifetime associations.
struct SessionConfirmationProof {
    SessionConfirmationIdentity identity;
    SessionConfirmationPresenter presenter;
    SessionConfirmationScope scope;
    SessionConfirmationProjection projection;
    bool operator==(const SessionConfirmationProof &) const noexcept = default;
};

struct SessionConfirmationDecisionProof {
    SessionConfirmationProof proof;
    SessionConfirmationDecision decision{SessionConfirmationDecision::Cancel};
    std::uint64_t input_sequence{};
    std::uint32_t protocol_serial{};
    bool operator==(const SessionConfirmationDecisionProof &) const noexcept = default;
};

bool ValidSessionConfirmationIdentity(SessionConfirmationIdentity) noexcept;
bool ValidSessionConfirmationProjection(SessionConfirmationProjection) noexcept;
bool ValidSessionConfirmationCancelReason(SessionConfirmationCancelReason) noexcept;
bool ValidateSessionConfirmationPlan(const SessionConfirmationPlan &) noexcept;
bool ValidateSessionConfirmationProof(const SessionConfirmationProof &) noexcept;
bool ValidateSessionConfirmationDecisionProof(const SessionConfirmationDecisionProof &) noexcept;

} // namespace prism::contracts
