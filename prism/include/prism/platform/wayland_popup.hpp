#pragma once

#include "prism/contracts/events.hpp"
#include "prism/contracts/popup_positioner.hpp"
#include "prism/contracts/surface_effect.hpp"
#include "prism/platform/presentation.hpp"
#include "prism/platform/surface_effect_capabilities.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

struct wl_buffer;
struct wl_surface;
struct xdg_popup;
struct xdg_surface;
struct wp_presentation_feedback;
struct wl_callback;
struct wl_output;
struct prism_surface_effect_v1;

namespace prism::platform {
class WaylandWindow;
enum class SubmitResult;

struct WaylandPopupTarget {
    std::uint64_t surface_lifetime_id{};
    std::uint64_t configure_generation{};

    explicit operator bool() const noexcept
    {
        return surface_lifetime_id && configure_generation;
    }

    bool operator==(const WaylandPopupTarget &) const = default;
};

struct WaylandPopupInput {
    WaylandPopupTarget target;
    PixelSubmissionId submission;
    contracts::WindowEvent event;
};

struct WaylandPopupPresentation {
    WaylandPopupTarget target;
    PixelPresentation presentation;
};

struct WaylandPopupFrameReady {
    WaylandPopupTarget target;
    PixelSubmissionId submission;
};

enum class WaylandPopupCloseReason {
    Requested,
    ParentClosed,
    CompositorDismissed,
    ProtocolFailure
};

struct WaylandPopupConfigure {
    // Functional window geometry includes the body and connecting neck, while
    // excluding shadows. Position is relative to the parent's window geometry;
    // it does not include this geometry's offset in the buffer.
    contracts::LogicalRect bounds;
    std::uint32_t serial{};
    std::uint64_t generation{};
};

struct WaylandPopupBufferLayout {
    // Buffer pixels and surface logical coordinates coincide at scale 1.
    contracts::BufferSize buffer_size;
    contracts::LogicalRect window_geometry;
    std::uint64_t configure_generation{};

    bool operator==(const WaylandPopupBufferLayout &other) const noexcept
    {
        return buffer_size.width == other.buffer_size.width &&
               buffer_size.height == other.buffer_size.height &&
               window_geometry == other.window_geometry &&
               configure_generation == other.configure_generation;
    }
};

struct WaylandPopupClosed {
    WaylandPopupCloseReason reason{WaylandPopupCloseReason::Requested};
    WaylandPopupTarget target;
};

using WaylandPopupEvent =
    std::variant<WaylandPopupConfigure, WaylandPopupClosed, WaylandPopupFrameReady>;

// A non-grabbing xdg_popup on its parent's connection. All methods and callbacks
// run on the parent's Wayland owner thread; only WaylandWindow::Pump dispatches.
// The parent closes registered children before releasing its borrowed proxies.
class WaylandPopup {
public:
    WaylandPopup() = default;
    ~WaylandPopup();
    WaylandPopup(const WaylandPopup &) = delete;
    WaylandPopup &operator=(const WaylandPopup &) = delete;
    WaylandPopup(WaylandPopup &&) = delete;
    WaylandPopup &operator=(WaylandPopup &&) = delete;

    // The parent currently commits its full surface as window geometry. Open
    // requires a mapped parent whose latest logical size has a pixel commit.
    bool Open(WaylandWindow &, const contracts::PopupPositionerRequest &);
    void Close() noexcept;
    void SetEventHandler(std::function<void(const WaylandPopupEvent &)> handler);
    void SetInputHandler(std::function<void(const WaylandPopupInput &)> handler);
    void SetPresentationHandler(std::function<void(const WaylandPopupPresentation &)> handler);
    WaylandPopupTarget Target() const noexcept;
    PixelSubmissionId LastPixelSubmission() const noexcept;
    bool FrameCallbackPending() const noexcept;
    bool PresentationPending(PixelSubmissionId submission) const noexcept;
    // All state must belong to the expected native lifetime/configure. Pixels
    // are committed only after requesting this child's frame and feedback.
    // AwaitFrame does not invoke commit_pixels. Failure retires the child.
    // The commit callback must not Open, Close, Pump or destroy an owner.
    SubmitResult SubmitPixels(const WaylandPopupTarget &expected,
                              const std::function<bool()> &commit_pixels);
    // Metadata updates require an already committed compatible pixel layout.
    // No frame/feedback is created; input retains the last real pixel ID.
    // None is checked metadata adoption; incompatible/unmapped returns AwaitFrame.
    SubmitResult SubmitState(const WaylandPopupTarget &expected);
    // Stages only. The final pixel commit adopts this mask; shadows remain
    // excluded by the supplied functional regions.
    bool SetInputRegions(const WaylandPopupTarget &expected,
                         std::span<const contracts::SurfaceInputRegion> regions);
    WaylandSurfaceEffectCapabilities SurfaceEffectCapabilities() const noexcept;
    // Surface-local effects are staged for this lifetime/configure. Invalid or
    // unsupported lists fail atomically; a contour never becomes a rectangle.
    // Pixels/State adopt the complete list, while checked None retains it.
    bool SetSurfaceEffects(const WaylandPopupTarget &expected,
                           std::span<const contracts::SurfaceEffectRegion> regions);
    // A single backend release, before destroying this lifetime's proxies.
    // Keep its owner alive; it must not throw or call any Wayland owner Close,
    // Open, Pump or destructor. Only release GPU/WSI resources here. Closed
    // observers run after this hook.
    void SetBeforeCloseHandler(std::function<void()> handler);
    bool IsConfigured() const noexcept;
    bool IsClosed() const noexcept;
    const std::optional<WaylandPopupConfigure> &Configure() const noexcept;
    const std::optional<WaylandPopupBufferLayout> &BufferLayout() const noexcept;
    // Window geometry has protocol integer coordinates, equals the configured
    // size and lies inside a buffer of at most 4096 pixels on each axis.
    // Set/Reset only stage window geometry; neither operation commits pixels.
    // A new configure replaces the prior layout with geometry origin (0, 0).
    bool SetBufferLayout(const WaylandPopupBufferLayout &);
    bool ResetBufferLayout(std::uint64_t configure_generation);
    // Borrowed only after configure. It becomes null on dismissal/parent close.
    // EGL callers must SetBufferLayout for the current configure, size their
    // WSI to its buffer_size, then MakeCurrent before drawing/swapping. Raw EGL
    // posting is not automatically gated by these layout methods.
    wl_surface *Surface() const noexcept;
    // The caller owns the buffer. Size must match the current buffer layout;
    // no attach/commit is allowed before configure or after the lifetime ends.
    // With no explicit Set, this applies the default geometry at origin.
    bool AttachBuffer(wl_buffer *, int width, int height);

private:
    friend class WaylandWindow;
    static void FrameDone(void *, struct wl_callback *, std::uint32_t);
    static void PresentationOutput(void *, struct wp_presentation_feedback *, struct wl_output *);
    static void PresentationDone(void *, struct wp_presentation_feedback *, std::uint32_t,
                                 std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                 std::uint32_t, std::uint32_t);
    static void PresentationDiscarded(void *, struct wp_presentation_feedback *);
    bool Matches(const WaylandPopupTarget &) const noexcept;
    bool PresentationCapacityAvailable() const noexcept;
    bool TrackPresentation(PixelSubmissionId);
    void FinishPresentation(struct wp_presentation_feedback *, PresentationOutcome);

    struct RetiredSubmission {
        std::array<std::optional<WaylandPopupInput>, 2> inputs;
        std::array<std::optional<WaylandPopupPresentation>, 8> presentations;
    };

    RetiredSubmission RetireSubmission() noexcept;
    void DeliverRetiredSubmission(const RetiredSubmission &) noexcept;
    void RevokeSubmission() noexcept;
    std::array<std::optional<WaylandPopupInput>, 2> TakeInputCancellation() noexcept;
    void CancelInput() noexcept;
    void RouteInput(contracts::WindowEvent) noexcept;
    void EmitInput(const WaylandPopupInput &) noexcept;
    void EmitPresentation(const WaylandPopupPresentation &) noexcept;
    bool CommitAttachedBuffer();
    void ResetSurfaceEffects() noexcept;
    void CloseSurfaceEffects() noexcept;
    static void SurfaceConfigure(void *, xdg_surface *, std::uint32_t);
    static void PopupConfigure(void *, xdg_popup *, std::int32_t, std::int32_t, std::int32_t,
                               std::int32_t);
    static void PopupDone(void *, xdg_popup *);
    static void PopupRepositioned(void *, xdg_popup *, std::uint32_t);
    void Close(WaylandPopupCloseReason, bool notify = true) noexcept;
    void Emit(const WaylandPopupEvent &) noexcept;

    WaylandWindow *parent_{};
    wl_surface *surface_{};
    xdg_surface *xdg_surface_{};
    xdg_popup *popup_{};
    std::optional<contracts::LogicalRect> pending_bounds_;
    std::optional<WaylandPopupConfigure> configure_;
    std::optional<WaylandPopupBufferLayout> buffer_layout_;
    std::optional<WaylandPopupBufferLayout> committed_buffer_layout_;
    std::function<void(const WaylandPopupEvent &)> event_handler_;
    std::function<void()> before_close_handler_;
    std::function<void(const WaylandPopupInput &)> input_handler_;
    std::function<void(const WaylandPopupPresentation &)> presentation_handler_;

    struct PendingPresentation {
        struct wp_presentation_feedback *handle{};
        WaylandPopupTarget target;
        PixelSubmissionId submission;
        bool committed{};
    };

    std::array<PendingPresentation, 8> feedbacks_{};
    struct wl_callback *frame_callback_{};
    WaylandPopupTarget frame_target_, committed_target_;
    PixelSubmissionId last_pixel_submission_, frame_submission_;
    std::vector<contracts::SurfaceInputRegion> sent_input_;
    struct prism_surface_effect_v1 *surface_effect_{};
    std::vector<contracts::SurfaceEffectRegion> sent_effects_;
    wl_buffer *attached_buffer_{};
    int attached_width_{}, attached_height_{};
    std::uint64_t surface_lifetime_id_{};
    bool input_sent_{};
    bool submitting_{};
    bool state_pending_{};
    std::shared_ptr<bool> callback_alive_{std::make_shared<bool>(true)};
    std::uint64_t configure_generation_{};
    std::uint64_t lifetime_generation_{};
    bool closed_{true};
    bool closing_{};
    bool buffer_layout_applied_{};
};
} // namespace prism::platform
