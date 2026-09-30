#pragma once
#include "prism/contracts/layout_control.hpp"
#include "prism/contracts/layout_snapshot.hpp"
#include "prism/contracts/theme.hpp"
#include "prism/launch/shell_permit.hpp"
#include <memory>
#include <span>
#include <vector>

namespace prism::launch {
enum class ControlType : std::uint16_t {
    Ready = 1,
    Grant,
    Registered,
    Revoke,
    Activate,
    Activated,
    Mapped,
    Unmapped,
    InstallTheme,
    ThemeApplied,
    LayoutSubscribe,
    LayoutSnapshot,
    LayoutControl,
    LayoutControlResult
};

struct ControlMessage {
    ControlType type{ControlType::Ready};
    ShellPermit permit;
    bool success{};
    contracts::ThemeSnapshot theme;
    contracts::ThemeApplied theme_applied;
    bool layout_subscribe{};
    contracts::LayoutSnapshot layout_snapshot;
    contracts::LayoutControlRequest control_request;
    contracts::LayoutControlResult control_result;
};

// Owned immutable cache for one authenticated WM connection. Reset on disconnect.
class LayoutSnapshotCache {
public:
    void Reset(std::uint64_t session = 0);
    void Accept(contracts::LayoutSnapshot snapshot);
    std::shared_ptr<const contracts::LayoutSnapshot> Current() const noexcept;

private:
    std::uint64_t session_{};
    std::shared_ptr<const contracts::LayoutSnapshot> current_;
};

std::vector<std::uint8_t> EncodeControl(const ControlMessage &message);
std::size_t ControlFrameSize(std::span<const std::uint8_t> bytes);
ControlMessage DecodeControl(std::span<const std::uint8_t> bytes);
std::uint64_t MonotonicNs();
void RandomBytes(std::span<std::uint8_t> bytes);
// Inherited socketpair endpoints were created by the session supervisor.
void VerifyControlPeer(int fd, int parent_pid);
} // namespace prism::launch
