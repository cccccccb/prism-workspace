#pragma once
#include "prism/contracts/theme.hpp"
#include "prism/launch/shell_permit.hpp"
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
    ThemeApplied
};

struct ControlMessage {
    ControlType type{ControlType::Ready};
    ShellPermit permit;
    bool success{};
    contracts::ThemeSnapshot theme;
    contracts::ThemeApplied theme_applied;
};

std::vector<std::uint8_t> EncodeControl(const ControlMessage &message);
std::size_t ControlFrameSize(std::span<const std::uint8_t> bytes);
ControlMessage DecodeControl(std::span<const std::uint8_t> bytes);
std::uint64_t MonotonicNs();
void RandomBytes(std::span<std::uint8_t> bytes);
// Inherited socketpair endpoints were created by the session supervisor.
void VerifyControlPeer(int fd, int parent_pid);
} // namespace prism::launch
