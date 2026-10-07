#include "prism/contracts/rounded_region.hpp"
#include "prism/platform/wayland_popup.hpp"
#include "prism/platform/wayland_window.hpp"

#include <utility>
#include <wayland-client.h>

namespace prism::platform {
void WaylandPopup::SetInputHandler(std::function<void(const WaylandPopupInput &)> handler)
{
    input_handler_ = std::move(handler);
}

void WaylandPopup::EmitInput(const WaylandPopupInput &event) noexcept
{
    const auto alive = callback_alive_;
    const auto lifetime = lifetime_generation_;
    try {
        const auto handler = input_handler_;
        if (handler) {
            handler(event);
        }
    } catch (...) {
        if (*alive && lifetime == lifetime_generation_) {
            Close(WaylandPopupCloseReason::ProtocolFailure);
        }
    }
}

void WaylandPopup::RouteInput(contracts::WindowEvent event) noexcept
{
    if (!Matches(committed_target_) || !last_pixel_submission_) {
        return;
    }
    EmitInput({committed_target_, last_pixel_submission_, std::move(event)});
}

std::array<std::optional<WaylandPopupInput>, 2> WaylandPopup::TakeInputCancellation() noexcept
{
    std::array<std::optional<WaylandPopupInput>, 2> events;
    const auto target = std::exchange(committed_target_, {});
    if (!target || !last_pixel_submission_ || !parent_) {
        return events;
    }
    const auto submission = last_pixel_submission_;
    const auto time = WaylandWindow::InputTimeNs();
    if (parent_->pointer_focus_surface_ == surface_) {
        events[0] = WaylandPopupInput{
            target, submission,
            contracts::PointerCancelEvent{contracts::WindowId{1}, time, parent_->PointerSource()}};
    }
    if (parent_->keyboard_focus_surface_ == surface_) {
        events[1] = WaylandPopupInput{
            target, submission,
            contracts::FocusEvent{contracts::WindowId{1}, false, parent_->KeyboardSource(), true}};
    }
    return events;
}

void WaylandPopup::CancelInput() noexcept
{
    const auto alive = callback_alive_;
    const auto events = TakeInputCancellation();
    for (const auto &event : events) {
        if (event) {
            EmitInput(*event);
            if (!*alive) {
                return;
            }
        }
    }
}

bool WaylandPopup::SetInputRegions(const WaylandPopupTarget &expected,
                                   std::span<const contracts::SurfaceInputRegion> regions)
{
    if (!Matches(expected) || !buffer_layout_ || !parent_->compositor_ || regions.size() > 4096) {
        return false;
    }
    std::vector<contracts::SurfaceInputRegion> next(regions.begin(), regions.end());
    if (input_sent_ && next == sent_input_) {
        return true;
    }
    std::vector<contracts::LogicalRect> rectangles;
    const auto &size = buffer_layout_->buffer_size;
    const contracts::SurfaceInputRegion viewport{{0, 0, double(size.width), double(size.height)},
                                                 0};
    try {
        for (const auto &shape : next) {
            const contracts::SurfaceInputRegion intersection[]{shape, viewport};
            auto spans = contracts::RasterizeRoundedIntersection(intersection);
            rectangles.insert(rectangles.end(), spans.begin(), spans.end());
        }
    } catch (...) {
        return false;
    }

    auto *input = wl_compositor_create_region(parent_->compositor_);
    if (!input) {
        return false;
    }
    for (const auto &rect : rectangles) {
        wl_region_add(input, static_cast<int>(rect.x), static_cast<int>(rect.y),
                      static_cast<int>(rect.width), static_cast<int>(rect.height));
    }
    wl_surface_set_input_region(surface_, input);
    wl_region_destroy(input);
    sent_input_ = std::move(next);
    input_sent_ = true;
    state_pending_ = true;
    return true;
}
} // namespace prism::platform
