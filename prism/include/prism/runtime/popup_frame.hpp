#pragma once

#include "prism/platform/presentation.hpp"
#include "prism/platform/wayland_popup.hpp"
#include "prism/runtime/popup_surface.hpp"
#include "prism/runtime/render_resource.hpp"
#include "prism/runtime/render_worker_lifecycle.hpp"
#include "prism/runtime/ui_load.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace prism::runtime {

// Host-owned native provenance, separate from the Scene's logical popup epoch.
// Adoption sequence is zero in candidates; successful Pixels/State/checked
// None may advance it. The actual pixel submission ID remains separate.
struct PopupFramePacket {
    UiLoadId ui{};
    RenderWorkerGeneration worker{};
    PopupSurfaceIdentity identity{};
    std::uint64_t sequence{}, resource_epoch{};
    std::shared_ptr<const PopupSurfacePlan> plan;
    std::shared_ptr<const std::vector<ImageVersion>> image_uses;
};

// These events share the root's ordered input/configure acknowledgment stream.
struct PopupConfigureEvent {
    UiLoadId ui{};
    PopupSurfaceIdentity identity{};
    std::uint64_t sequence{};
    std::shared_ptr<const PopupSurfaceRequest> request;
    PopupSurfaceConfigure configure{};
};

struct PopupClosedEvent {
    UiLoadId ui{};
    PopupSurfaceIdentity identity{};
    std::uint64_t sequence{};
    std::shared_ptr<const PopupSurfaceRequest> request;
    platform::WaylandPopupCloseReason reason{platform::WaylandPopupCloseReason::Requested};
};

struct PopupInputEvent {
    UiLoadId ui{};
    PopupSurfaceIdentity identity{};
    std::uint64_t sequence{};
    contracts::WindowEvent event;
    std::shared_ptr<const InputSnapshot> input_snapshot;
};

// Successful child pixels do not advance the root UI/Preview/Master milestone.
struct PopupSubmittedEvent {
    UiLoadId ui{};
    PopupSurfaceIdentity identity{};
    std::uint64_t sequence{};
    std::shared_ptr<const PopupFramePacket> frame;
    bool feedback_expected{};
    bool pixels{true};
    platform::PixelSubmissionId pixel_submission{};
};

struct PopupPresentationEvent {
    UiLoadId ui{};
    PopupSurfaceIdentity identity{};
    platform::PixelPresentation presentation;
};

struct RejectPopupCommand {
    UiLoadId ui{};
    PopupSurfaceIdentity identity{};
};

} // namespace prism::runtime
