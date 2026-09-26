#pragma once
#include "prism/launch/protocol.hpp"
namespace prism::launch {
struct WorkerReady { std::uint64_t preparation_ns{}; };
struct WorkerBind { contracts::LaunchRequest request; contracts::InstanceId instance; };
struct WorkerReply { contracts::LaunchEvent event; };
using WorkerMessage = std::variant<WorkerReady, WorkerBind, contracts::LaunchEvent,
    contracts::LaunchRequest, WorkerReply, contracts::LaunchCancel, contracts::InstanceSubscribe, contracts::InstanceUpdate>;
std::vector<std::uint8_t> EncodeWorker(const WorkerMessage& message);
std::size_t WorkerFrameSize(std::span<const std::uint8_t> bytes);
WorkerMessage DecodeWorker(std::span<const std::uint8_t> bytes);
} // namespace prism::launch
