#include "prism/platform/wayland_popup.hpp"
#include "prism/platform/wayland_window.hpp"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <utility>
#include <wayland-client.h>

namespace prism::platform {
namespace {
std::uint64_t AllocatePopupLifetime()
{
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    while (value && value != std::numeric_limits<std::uint64_t>::max()) {
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) {
            return value;
        }
    }
    return 0;
}

WaylandPopupBufferLayout DefaultBufferLayout(const WaylandPopupConfigure &configure)
{
    const auto &bounds = configure.bounds;
    return {{static_cast<std::uint32_t>(bounds.width), static_cast<std::uint32_t>(bounds.height)},
            {0, 0, bounds.width, bounds.height},
            configure.generation};
}

bool ProtocolInteger(double value)
{
    return std::isfinite(value) && value >= 0 && value <= 4096 && std::trunc(value) == value;
}

bool ValidBufferLayout(const WaylandPopupBufferLayout &layout,
                       const WaylandPopupConfigure &configure)
{
    const auto &buffer = layout.buffer_size;
    const auto &geometry = layout.window_geometry;
    return layout.configure_generation == configure.generation && buffer.width > 0 &&
           buffer.height > 0 && buffer.width <= 4096 && buffer.height <= 4096 &&
           ProtocolInteger(geometry.x) && ProtocolInteger(geometry.y) &&
           ProtocolInteger(geometry.width) && ProtocolInteger(geometry.height) &&
           geometry.width == configure.bounds.width && geometry.height == configure.bounds.height &&
           geometry.x + geometry.width <= buffer.width &&
           geometry.y + geometry.height <= buffer.height;
}

std::uint32_t Anchor(const contracts::PopupPositioner &position)
{
    using Horizontal = contracts::PopupHorizontalAlignment;
    const bool below = position.vertical_preference == contracts::PopupVerticalPreference::Below;
    if (position.horizontal_alignment == Horizontal::Start) {
        return below ? XDG_POSITIONER_ANCHOR_BOTTOM_LEFT : XDG_POSITIONER_ANCHOR_TOP_LEFT;
    }
    if (position.horizontal_alignment == Horizontal::End) {
        return below ? XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT : XDG_POSITIONER_ANCHOR_TOP_RIGHT;
    }
    return below ? XDG_POSITIONER_ANCHOR_BOTTOM : XDG_POSITIONER_ANCHOR_TOP;
}

std::uint32_t Gravity(const contracts::PopupPositioner &position)
{
    using Horizontal = contracts::PopupHorizontalAlignment;
    const bool below = position.vertical_preference == contracts::PopupVerticalPreference::Below;
    if (position.horizontal_alignment == Horizontal::Start) {
        return below ? XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT : XDG_POSITIONER_GRAVITY_TOP_RIGHT;
    }
    if (position.horizontal_alignment == Horizontal::End) {
        return below ? XDG_POSITIONER_GRAVITY_BOTTOM_LEFT : XDG_POSITIONER_GRAVITY_TOP_LEFT;
    }
    return below ? XDG_POSITIONER_GRAVITY_BOTTOM : XDG_POSITIONER_GRAVITY_TOP;
}

void Position(xdg_positioner *positioner, const contracts::PopupPositioner &position)
{
    xdg_positioner_set_size(positioner, position.width, position.height);
    const auto &anchor = position.anchor;
    xdg_positioner_set_anchor_rect(positioner, anchor.x, anchor.y, anchor.width, anchor.height);
    xdg_positioner_set_anchor(positioner, Anchor(position));
    xdg_positioner_set_gravity(positioner, Gravity(position));
    const bool below = position.vertical_preference == contracts::PopupVerticalPreference::Below;
    xdg_positioner_set_offset(positioner, 0, below ? position.gap : -position.gap);
    xdg_positioner_set_constraint_adjustment(positioner,
                                             XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
                                                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                                                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
                                                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X |
                                                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y);
}
} // namespace

WaylandPopup::~WaylandPopup()
{
    *callback_alive_ = false;
    event_handler_ = {};
    input_handler_ = {};
    presentation_handler_ = {};
    Close();
}

bool WaylandPopup::Open(WaylandWindow &parent, const contracts::PopupPositionerRequest &request)
{
    if (!closed_ || closing_ || parent.closing_ || parent.deferred_close_ ||
        !parent.IsConfigured() || !parent.IsMapped() || parent.failed_ ||
        parent.IsCloseRequested() || !parent.display_ || !parent.compositor_ || !parent.shell_ ||
        !parent.xdg_surface_ || parent.Metrics().logical_size != parent.committed_logical_size_ ||
        lifetime_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    const auto size = parent.committed_logical_size_;
    const auto position =
        contracts::PreparePopupPositioner(request, {0, 0, size.width, size.height});
    if (!position) {
        return false;
    }

    const auto native_lifetime = AllocatePopupLifetime();
    if (!native_lifetime) {
        return false;
    }

    parent.popups_.push_back(this);
    surface_lifetime_id_ = native_lifetime;
    parent_ = &parent;
    ++lifetime_generation_;
    closed_ = false;
    configure_.reset();
    buffer_layout_.reset();
    buffer_layout_applied_ = false;
    pending_bounds_.reset();

    auto *positioner = xdg_wm_base_create_positioner(parent.shell_);
    if (positioner) {
        Position(positioner, *position);
        surface_ = wl_compositor_create_surface(parent.compositor_);
    }
    if (surface_) {
        xdg_surface_ = xdg_wm_base_get_xdg_surface(parent.shell_, surface_);
    }
    if (xdg_surface_) {
        popup_ = xdg_surface_get_popup(xdg_surface_, parent.xdg_surface_, positioner);
    }
    if (positioner) {
        xdg_positioner_destroy(positioner);
    }
    if (!surface_ || !xdg_surface_ || !popup_) {
        Close(WaylandPopupCloseReason::ProtocolFailure);
        return false;
    }

    static const xdg_surface_listener surface_listener{.configure = SurfaceConfigure};
    static const xdg_popup_listener popup_listener{
        .configure = PopupConfigure, .popup_done = PopupDone, .repositioned = PopupRepositioned};
    if (xdg_surface_add_listener(xdg_surface_, &surface_listener, this) < 0 ||
        xdg_popup_add_listener(popup_, &popup_listener, this) < 0) {
        Close(WaylandPopupCloseReason::ProtocolFailure);
        return false;
    }

    wl_surface_commit(surface_);
    return true;
}

void WaylandPopup::SetEventHandler(std::function<void(const WaylandPopupEvent &)> handler)
{
    event_handler_ = std::move(handler);
}

void WaylandPopup::SetBeforeCloseHandler(std::function<void()> handler)
{
    before_close_handler_ = std::move(handler);
}

bool WaylandPopup::IsConfigured() const noexcept
{
    return !closed_ && configure_.has_value();
}

bool WaylandPopup::IsClosed() const noexcept
{
    return closed_;
}

const std::optional<WaylandPopupConfigure> &WaylandPopup::Configure() const noexcept
{
    return configure_;
}

const std::optional<WaylandPopupBufferLayout> &WaylandPopup::BufferLayout() const noexcept
{
    return buffer_layout_;
}

bool WaylandPopup::SetBufferLayout(const WaylandPopupBufferLayout &layout)
{
    if (!IsConfigured() || !parent_ || parent_->closing_ || parent_->deferred_close_ ||
        !ValidBufferLayout(layout, *configure_)) {
        return false;
    }
    if (buffer_layout_applied_ && buffer_layout_ && *buffer_layout_ == layout) {
        return true;
    }

    const auto &geometry = layout.window_geometry;
    xdg_surface_set_window_geometry(xdg_surface_, static_cast<int>(geometry.x),
                                    static_cast<int>(geometry.y), static_cast<int>(geometry.width),
                                    static_cast<int>(geometry.height));
    buffer_layout_ = layout;
    buffer_layout_applied_ = true;
    state_pending_ = true;
    return true;
}

bool WaylandPopup::ResetBufferLayout(std::uint64_t configure_generation)
{
    if (!IsConfigured() || configure_generation != configure_->generation) {
        return false;
    }
    return SetBufferLayout(DefaultBufferLayout(*configure_));
}

wl_surface *WaylandPopup::Surface() const noexcept
{
    return IsConfigured() ? surface_ : nullptr;
}

bool WaylandPopup::AttachBuffer(wl_buffer *buffer, int width, int height)
{
    if (!buffer || !IsConfigured() || !parent_ || parent_->closing_ || parent_->deferred_close_ ||
        !buffer_layout_ || width <= 0 || height <= 0 ||
        static_cast<std::uint32_t>(width) != buffer_layout_->buffer_size.width ||
        static_cast<std::uint32_t>(height) != buffer_layout_->buffer_size.height) {
        return false;
    }
    if (!buffer_layout_applied_ && !SetBufferLayout(*buffer_layout_)) {
        return false;
    }

    attached_buffer_ = buffer;
    attached_width_ = width;
    attached_height_ = height;
    const auto result =
        SubmitPixels(Target(), std::bind_front(&WaylandPopup::CommitAttachedBuffer, this));
    attached_buffer_ = nullptr;
    return result == SubmitResult::Pixels;
}

void WaylandPopup::Close() noexcept
{
    Close(WaylandPopupCloseReason::Requested);
}

void WaylandPopup::Close(WaylandPopupCloseReason reason, bool notify) noexcept
{
    if (closed_) {
        return;
    }
    const auto target = Target();
    const auto alive = callback_alive_;
    closed_ = true;
    closing_ = true;
    const auto retired = RetireSubmission();
    auto *parent = parent_;
    if (parent) {
        std::erase(parent->popups_, this);
        if (parent->pointer_focus_surface_ == surface_) {
            parent->pointer_focus_surface_ = nullptr;
        }
        if (parent->keyboard_focus_surface_ == surface_) {
            parent->keyboard_focus_surface_ = nullptr;
        }
        parent_ = nullptr;
    }

    const bool parent_closing = parent ? std::exchange(parent->closing_, true) : false;
    auto before_close = std::move(before_close_handler_);
    before_close_handler_ = {};
    if (before_close) {
        before_close();
    }
    if (parent) {
        parent->closing_ = parent_closing;
    }

    CloseSurfaceEffects();

    if (popup_) {
        xdg_popup_destroy(popup_);
    }
    if (xdg_surface_) {
        xdg_surface_destroy(xdg_surface_);
    }
    if (surface_) {
        wl_surface_destroy(surface_);
    }
    popup_ = nullptr;
    xdg_surface_ = nullptr;
    surface_ = nullptr;
    pending_bounds_.reset();
    buffer_layout_.reset();
    buffer_layout_applied_ = false;
    sent_input_.clear();
    input_sent_ = false;
    closing_ = false;

    DeliverRetiredSubmission(retired);
    if (!*alive) {
        return;
    }
    if (notify) {
        Emit(WaylandPopupClosed{reason, target});
    }
}

void WaylandPopup::Emit(const WaylandPopupEvent &event) noexcept
{
    const auto alive = callback_alive_;
    const auto lifetime = lifetime_generation_;
    try {
        const auto handler = event_handler_;
        if (handler) {
            handler(event);
        }
    } catch (...) {
        if (*alive && lifetime == lifetime_generation_) {
            Close(WaylandPopupCloseReason::ProtocolFailure);
        }
    }
}

void WaylandPopup::PopupConfigure(void *data, xdg_popup *, std::int32_t x, std::int32_t y,
                                  std::int32_t width, std::int32_t height)
{
    auto &self = *static_cast<WaylandPopup *>(data);
    if (self.closed_) {
        return;
    }
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        self.Close(WaylandPopupCloseReason::ProtocolFailure);
        return;
    }
    self.pending_bounds_ =
        contracts::LogicalRect{double(x), double(y), double(width), double(height)};
}

void WaylandPopup::SurfaceConfigure(void *data, xdg_surface *surface, std::uint32_t serial)
{
    auto &self = *static_cast<WaylandPopup *>(data);
    if (self.closed_) {
        return;
    }
    if ((!self.pending_bounds_ && !self.configure_) ||
        self.configure_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        self.Close(WaylandPopupCloseReason::ProtocolFailure);
        return;
    }

    const auto alive = self.callback_alive_;
    self.RevokeSubmission();
    if (!*alive || self.closed_) {
        return;
    }
    const auto bounds = self.pending_bounds_ ? *self.pending_bounds_ : self.configure_->bounds;
    xdg_surface_ack_configure(surface, serial);
    self.configure_ = WaylandPopupConfigure{bounds, serial, ++self.configure_generation_};
    self.buffer_layout_ = DefaultBufferLayout(*self.configure_);
    self.buffer_layout_applied_ = false;
    self.pending_bounds_.reset();
    self.sent_input_.clear();
    self.input_sent_ = false;
    self.ResetSurfaceEffects();

    self.Emit(*self.configure_);
}

void WaylandPopup::PopupDone(void *data, xdg_popup *)
{
    static_cast<WaylandPopup *>(data)->Close(WaylandPopupCloseReason::CompositorDismissed);
}

void WaylandPopup::PopupRepositioned(void *, xdg_popup *, std::uint32_t)
{
}

void WaylandWindow::ClosePopups(bool notify) noexcept
{
    const auto reason =
        failed_ ? WaylandPopupCloseReason::ProtocolFailure : WaylandPopupCloseReason::ParentClosed;
    while (!popups_.empty()) {
        popups_.back()->Close(reason, notify);
    }
}
} // namespace prism::platform
