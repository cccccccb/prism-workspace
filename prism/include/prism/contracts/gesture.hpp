#pragma once

#include "prism/contracts/events.hpp"
#include <string>

namespace prism::contracts {

enum class GesturePhase : std::uint8_t { Begin, Update, End, Cancel };

// Owning UI-thread value. Positions are window logical coordinates; the serial
// is the original platform Down credential, not an authorization by itself.
struct GestureEvent {
    GesturePhase phase{GesturePhase::Begin};
    std::uint64_t id{}; // Process-wide, nonzero and never reused across UI replacements.
    NodeId node{};
    std::string action;
    InputSource source{};
    bool touch{};
    InputContactId contact{};
    std::uint32_t serial{};
    LogicalPoint start{};
    LogicalPoint position{};
    std::uint64_t time_ns{};
    std::uint64_t snapshot_scene{};
    std::uint64_t snapshot_version{};
};

} // namespace prism::contracts
