#pragma once
#include "prism/contracts/layout_control.hpp"
#include "prism/launch/protocol.hpp"

namespace prism::launch {
struct WorkerReady {
    std::uint64_t preparation_ns{};
};

struct WorkerBind {
    contracts::LaunchRequest request;
    contracts::InstanceId instance;
    bool deferred_presentation{};
};

struct WorkerReply {
    contracts::LaunchEvent event;
};

using WorkerMessage =
    std::variant<WorkerReady, WorkerBind, contracts::LaunchEvent, contracts::LaunchRequest,
                 WorkerReply, contracts::LaunchCancel, contracts::InstanceSubscribe,
                 contracts::InstanceUpdate, contracts::ThemeSnapshot, contracts::ThemeApplied,
                 contracts::ThemeRequest, contracts::ThemeEvent, contracts::LayoutSubscription,
                 contracts::LayoutStateEvent, contracts::LayoutControlRequest,
                 contracts::LayoutControlResult>;
std::vector<std::uint8_t> EncodeWorker(const WorkerMessage &message);
std::size_t WorkerFrameSize(std::span<const std::uint8_t> bytes);
WorkerMessage DecodeWorker(std::span<const std::uint8_t> bytes);
} // namespace prism::launch
