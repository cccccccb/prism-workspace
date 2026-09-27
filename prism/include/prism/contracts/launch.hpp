#pragma once
#include <cstdint>
#include <string>

namespace prism::contracts {
inline constexpr std::uint16_t kLaunchProtocolVersion = 1;
inline constexpr std::uint32_t kAppPackageVersion = 1;
inline constexpr std::uint32_t kRuntimeAbiVersion = 1;
inline constexpr std::uint32_t kMaxLaunchPayload = 65536;

struct RequestId {
    std::uint64_t value{};
    bool operator==(const RequestId &) const = default;
};

struct InstanceId {
    std::uint64_t value{};
    bool operator==(const InstanceId &) const = default;
};
enum class LaunchMode : std::uint8_t { ActivateOrCreate = 0, NewInstance = 1 };

// Public callers identify a registered package, never a command or Shell role.
struct LaunchRequest {
    RequestId request;
    std::string app_id;
    LaunchMode mode{LaunchMode::ActivateOrCreate};
    bool operator==(const LaunchRequest &) const = default;
};

struct LaunchCancel {
    RequestId request;
    bool operator==(const LaunchCancel &) const = default;
};

struct InstanceSubscribe {
    RequestId request;
};
enum class InstanceChange : std::uint8_t { Reset = 0, Running, Stopped, SnapshotDone };

struct InstanceUpdate {
    RequestId request;
    InstanceId instance;
    std::uint32_t pid{};
    InstanceChange change{InstanceChange::Reset};
    std::string app_id;
};
enum class LaunchMilestone : std::uint8_t {
    Accepted = 0,
    WorkerAssigned,
    RuntimeReady,
    SurfaceConfigured,
    FirstPresented,
    BackendReady,
    Failed,
    Exited,
    Activated
};
enum class LaunchError : std::uint16_t {
    None = 0,
    InvalidRequest,
    UnknownApplication,
    InvalidPackage,
    UnsupportedAbi,
    NoWorker,
    ModuleLoadFailed,
    RuntimeFailed,
    PresentationFailed,
    Cancelled,
    Timeout,
    SessionEnded
};

// BackendReady is independent of presentation. pid==0 before assignment.
struct LaunchEvent {
    RequestId request;
    InstanceId instance;
    std::uint32_t pid{};
    LaunchMilestone milestone{LaunchMilestone::Accepted};
    LaunchError error{LaunchError::None};
    // Exited only: 0..255 normal exit code, negative signal number (-1..-64).
    std::int32_t exit_code{};
    std::string detail;
    bool operator==(const LaunchEvent &) const = default;
};
} // namespace prism::contracts
