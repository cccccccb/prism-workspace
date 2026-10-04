#pragma once

#include "prism/contracts/layout_snapshot.hpp"
#include <span>

namespace prism::contracts {

inline constexpr std::uint16_t kLayoutControlVersion = 1;
inline constexpr std::size_t kLayoutControlPayload = 118;
inline constexpr std::size_t kWindowControlPayload = 126;
inline constexpr std::size_t kLayoutControlResultPayload = 77;
inline constexpr std::size_t kMaxLayoutStatePayload = kMaxLayoutSnapshotPayload + 11;
inline constexpr double kMaxLayoutControlCoordinate = 1000000.0;

enum class LayoutControlPhase : std::uint8_t { Begin, Update, End, Cancel };
enum class LayoutControlOperation : std::uint8_t { GroupGesture, BoundaryGesture, WindowGesture };
enum class LayoutControlIntent : std::uint8_t {
    None,
    EnterImmersive,
    ExitImmersive,
    ApplyBoundary,
    EnterWindowFullscreen,
    ExitWindowFullscreen,
    SplitHorizontal,
    SplitVertical
};
enum class LayoutInputKind : std::uint8_t { Pointer, Touch };
enum class LayoutControlStatus : std::uint8_t { Began, Updated, Ended, Cancelled, Rejected };
enum class LayoutControlError : std::uint8_t {
    None,
    Unauthorized,
    StaleSession,
    StaleTarget,
    StaleLayout,
    InvalidInput,
    Busy,
    InvalidSequence,
    UnknownSession,
    Unsupported,
    Expired,
    Disconnected
};

struct LayoutControlTarget {
    std::uint64_t wm_session{}, output{}, workspace{}, root{}, boundary{};
    std::uint64_t topology_revision{}, layout_revision{};
    std::uint64_t node{};
    bool operator==(const LayoutControlTarget &) const = default;
};

struct LayoutInputProof {
    LayoutInputKind kind{LayoutInputKind::Pointer};
    std::uint32_t serial{};
    std::int32_t contact{};
    bool operator==(const LayoutInputProof &) const = default;
};

struct LayoutControlRequest {
    std::uint64_t request{}, gesture{}, session{}, sequence{};
    LayoutControlPhase phase{LayoutControlPhase::Begin};
    LayoutControlOperation operation{LayoutControlOperation::GroupGesture};
    LayoutControlIntent intent{LayoutControlIntent::None};
    LayoutControlTarget target;
    LayoutInputProof input;
    // Window-local logical coordinates; not a trusted WM pointer position.
    LogicalPoint position;
    bool operator==(const LayoutControlRequest &) const = default;
};

struct LayoutControlResult {
    std::uint64_t request{}, gesture{}, session{}, sequence{};
    LayoutControlStatus status{LayoutControlStatus::Rejected};
    LayoutControlError error{LayoutControlError::None};
    std::uint64_t revision{}, topology_revision{}, layout_revision{};
    LogicalPoint position;
    // Only a successful terminal intent reports applied; this is not presentation.
    bool applied{};
    bool operator==(const LayoutControlResult &) const = default;
};

struct LayoutSubscription {
    std::uint64_t request{};
    bool enabled{true};
    bool operator==(const LayoutSubscription &) const = default;
};
enum class LayoutStateStatus : std::uint8_t { Current, Denied, Disconnected };

struct LayoutStateEvent {
    std::uint64_t subscription{};
    LayoutStateStatus status{LayoutStateStatus::Current};
    LayoutSnapshot snapshot;
    bool operator==(const LayoutStateEvent &) const = default;
};

bool AllowsLayoutIntent(LayoutControlOperation, LayoutControlIntent);
void ValidateLayoutControl(const LayoutControlRequest &);
std::vector<std::uint8_t> EncodeLayoutControl(const LayoutControlRequest &);
LayoutControlRequest DecodeLayoutControl(std::span<const std::uint8_t>);
std::vector<std::uint8_t> EncodeLayoutControlResult(const LayoutControlResult &);
LayoutControlResult DecodeLayoutControlResult(std::span<const std::uint8_t>);
std::vector<std::uint8_t> EncodeLayoutSubscription(const LayoutSubscription &);
LayoutSubscription DecodeLayoutSubscription(std::span<const std::uint8_t>);
std::vector<std::uint8_t> EncodeLayoutState(const LayoutStateEvent &);
LayoutStateEvent DecodeLayoutState(std::span<const std::uint8_t>);

} // namespace prism::contracts
