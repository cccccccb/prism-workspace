#pragma once

#include "prism/runtime/frame_packet.hpp"
#include "prism/runtime/image_resources.hpp"
#include "prism/runtime/render_resource.hpp"
#include "prism/runtime/render_worker_lifecycle.hpp"

#include <cstdint>
#include <memory>
#include <variant>

namespace prism::runtime {

struct FrameCommand {
    std::shared_ptr<const FramePacket> frame;
};

// A successful UI installation changes the render baseline and must stay
// ordered relative to resource registrations and candidate frames.
struct InstallUiCommand {
    UiLoadId ui{};
};

// Discard the current candidate for this UI load without changing the last
// successfully committed pixel frame or its damage baseline. A replacement
// frame follows after the UI has combined its pending scene changes.
struct InvalidateFrameCommand {
    UiLoadId ui{};
};

// The UI owns animation sampling. While enabled, a callback cannot resubmit
// the last candidate until the UI has answered the worker's frame opportunity.
struct SetAnimationSamplingCommand {
    UiLoadId ui{};
    bool active{};
};

// A null frame means that quantization produced no new visible pixels. The
// immutable ready packet travels in this ordered command, so an earlier
// candidate cannot be mistaken for the response to this opportunity.
struct AnswerFrameOpportunityCommand {
    UiLoadId ui{};
    RenderWorkerGeneration worker{};
    int configure_count{};
    std::uint64_t id{};
    std::shared_ptr<const FramePacket> frame;
};

enum class RenderRequestKind { Update, Redraw };

struct RequestRenderCommand {
    RenderRequestKind kind{RenderRequestKind::Update};
    bool deferred{};
};

// Acknowledge the highest ordered window event after its UI changes and frame
// have been published. The worker holds old visuals until this barrier arrives.
struct UiEventsProcessedCommand {
    std::uint64_t sequence{};
};

struct RegisterImageCommand {
    ImageVersion version;
    ImageLease pixels;
};

struct ReleaseImageCommand {
    ImageVersion version;
};

// Ordered UI-to-render values. Every control is a replacement barrier;
// only adjacent frames of the same UI load may replace each other.
using RenderCommand =
    std::variant<FrameCommand, InstallUiCommand, InvalidateFrameCommand, RequestRenderCommand,
                 UiEventsProcessedCommand, RegisterImageCommand, ReleaseImageCommand,
                 SetAnimationSamplingCommand, AnswerFrameOpportunityCommand>;

inline bool ReplaceFrameTail(const RenderCommand &older, const RenderCommand &newer) noexcept
{
    const auto *previous = std::get_if<FrameCommand>(&older);
    const auto *current = std::get_if<FrameCommand>(&newer);
    return previous && current && previous->frame && current->frame &&
           previous->frame->ui == current->frame->ui;
}

} // namespace prism::runtime
