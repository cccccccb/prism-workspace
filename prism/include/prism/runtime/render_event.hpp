#pragma once

#include "prism/contracts/events.hpp"
#include "prism/platform/presentation.hpp"
#include "prism/runtime/frame_packet.hpp"
#include "prism/runtime/popup_frame.hpp"
#include "prism/runtime/render_bridge_types.hpp"
#include "prism/runtime/render_resource.hpp"
#include "prism/runtime/render_worker_lifecycle.hpp"
#include "prism/runtime/ui_load.hpp"

#include <cstdint>
#include <memory>
#include <variant>

namespace prism::runtime {

struct SequencedWindowEvent {
    contracts::WindowEvent event;
    std::uint64_t sequence{};
    UiLoadId ui{};
    std::shared_ptr<const InputSnapshot> input_snapshot;
};

enum class SubmittedKind { None, State, Pixels };

// A successful platform submission, delivered to the UI owner before any
// presentation result for the same pixel submission. Zero submission means
// a State or checked-identical None result with no new pixel buffer.
struct SubmittedFrameEvent {
    UiLoadId ui{};
    std::uint64_t frame_sequence{};
    // The immutable packet actually consumed by this submission. The UI can
    // distinguish it from a later candidate even if tail frames were merged.
    std::shared_ptr<const FramePacket> frame;
    platform::PixelSubmissionId submission{};
    SubmittedKind kind{SubmittedKind::None};
    std::uint64_t scene_revision{};
    std::uint64_t pixels_revision{};
    std::uint64_t theme_generation{};
    bool feedback_expected{};
    bool metadata_prepared{};
};

// The backend accepted and submitted this version's texture upload. This is
// not a GPU completion fence. The UI may mark this exact version ready only
// after consuming the event in the ordered reverse stream.
struct ImageUploadedEvent {
    ImageVersion version;
};

// The backend no longer owns or uses this exact version. An old release is
// acknowledged without unregistering a newer version with the same ID.
struct ImageReleasedEvent {
    ImageVersion version;
};

// One non-coalesced opportunity per active UI/worker/configure generation.
// The UI samples current monotonic time and answers with an ordered packet or
// an explicit no-change result. Status snapshots are not submission permits.
struct FrameOpportunityEvent {
    UiLoadId ui{};
    RenderWorkerGeneration worker{};
    int configure_count{};
    std::uint64_t id{};
};

// One ordered reverse stream preserves configure/input, successful submission,
// presentation, resource acknowledgments and status across both owners.
using RenderEvent =
    std::variant<SequencedWindowEvent, SubmittedFrameEvent, platform::PixelPresentation,
                 ImageUploadedEvent, ImageReleasedEvent, RenderStatusEvent, FrameOpportunityEvent,
                 PopupConfigureEvent, PopupClosedEvent, PopupInputEvent, PopupSubmittedEvent,
                 PopupPresentationEvent>;

// Only adjacent motions from the same UI, applied geometry and source can
// merge. Touch also requires the same contact; Down/Up/Cancel/Frame are barriers.
inline bool ReplacePointerMotionTail(const RenderEvent &older, const RenderEvent &newer) noexcept
{
    const auto *previous_popup = std::get_if<PopupInputEvent>(&older);
    const auto *current_popup = std::get_if<PopupInputEvent>(&newer);
    if (previous_popup || current_popup) {
        if (!previous_popup || !current_popup || previous_popup->ui != current_popup->ui ||
            previous_popup->identity != current_popup->identity ||
            previous_popup->input_snapshot != current_popup->input_snapshot) {
            return false;
        }
        const auto *previous = std::get_if<contracts::PointerMotionEvent>(&previous_popup->event);
        const auto *current = std::get_if<contracts::PointerMotionEvent>(&current_popup->event);
        return previous && current && previous->source == current->source &&
               previous->window == current->window;
    }
    const auto *previous_event = std::get_if<SequencedWindowEvent>(&older);
    const auto *current_event = std::get_if<SequencedWindowEvent>(&newer);
    if (!previous_event || !current_event || previous_event->ui != current_event->ui ||
        previous_event->input_snapshot != current_event->input_snapshot) {
        return false;
    }

    const auto *previous = std::get_if<contracts::PointerMotionEvent>(&previous_event->event);
    const auto *current = std::get_if<contracts::PointerMotionEvent>(&current_event->event);
    if (previous || current) {
        return previous && current && previous->window == current->window &&
               previous->source == current->source;
    }

    const auto *previous_touch = std::get_if<contracts::TouchMotionEvent>(&previous_event->event);
    const auto *current_touch = std::get_if<contracts::TouchMotionEvent>(&current_event->event);
    return previous_touch && current_touch && previous_touch->window == current_touch->window &&
           previous_touch->source == current_touch->source &&
           previous_touch->contact == current_touch->contact;
}

// Status may replace only an adjacent older status. A configure, input,
// opportunity, submission, presentation or resource event remains an
// ordering barrier.
inline bool ReplaceStatusTail(const RenderEvent &older, const RenderEvent &newer) noexcept
{
    return std::holds_alternative<RenderStatusEvent>(older) &&
           std::holds_alternative<RenderStatusEvent>(newer);
}

} // namespace prism::runtime
