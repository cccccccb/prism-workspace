#pragma once
#include "prism/contracts/launch.hpp"
#include "prism/contracts/types.hpp"
#include <array>

namespace prism::launch {
using ShellToken = std::array<std::uint8_t, 32>;
// Internal launcher/WM contract. Never accepted through the public Launch API.
// Session identity, PID and token must come from trusted bootstrap/credentials.
struct ShellPermit {
    std::uint64_t session{};
    contracts::RequestId request;
    contracts::InstanceId instance;
    std::uint32_t pid{};
    contracts::WindowRole role{contracts::WindowRole::Toplevel};
    ShellToken token{};
    std::uint64_t expires_ns{}; // Compositor's monotonic clock.
};
// Grant is consumed once, before role mapping; failed checks do not consume it.
// Owner must destroy/revoke the grant on session/worker termination.
class ShellPermitGuard {
public:
    explicit ShellPermitGuard(ShellPermit permit);
    ShellPermitGuard(const ShellPermitGuard&) = delete;
    ShellPermitGuard& operator=(const ShellPermitGuard&) = delete;
    bool Consume(const ShellPermit& claim, std::uint64_t now_ns);
    void Revoke() { consumed_ = true; }
private:
    ShellPermit permit_;
    bool consumed_{};
};
} // namespace prism::launch
