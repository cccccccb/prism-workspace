#pragma once
#include "prism/contracts/launch.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace prism::launch {
using LaunchMessage = std::variant<contracts::LaunchRequest, contracts::LaunchEvent, contracts::LaunchCancel, contracts::InstanceSubscribe, contracts::InstanceUpdate>;
inline constexpr std::size_t kLaunchHeaderSize = 28;
// Explicit big-endian byte encoding, never memcpy a C++/C ABI object.
std::vector<std::uint8_t> EncodeMessage(const LaunchMessage& message);
// Returns zero until a complete header is available. Invalid headers throw.
std::size_t FrameSize(std::span<const std::uint8_t> bytes);
// Requires exactly one complete frame; trailing/short/invalid data throw.
LaunchMessage DecodeMessage(std::span<const std::uint8_t> bytes);
} // namespace prism::launch
