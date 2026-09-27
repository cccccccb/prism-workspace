#pragma once

#include "prism/contracts/events.hpp"
#include "prism/contracts/surface_effect.hpp"
#include "prism/platform/presentation.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <wayland-client.h>

struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_shm;
struct wl_seat;
struct wl_pointer;
struct wl_keyboard;
struct wl_surface;
struct wl_callback;
struct wp_presentation;
struct wp_presentation_feedback;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;
struct prism_surface_effect_manager_v1;
struct prism_surface_effect_v1;

namespace prism::platform {

enum class SubmitResult { None, State, Pixels, Failed };

struct SubmitRequest {
    wl_display *display{};
    wl_surface *surface{};
    int width{}, height{};
    bool allow_pixels{}, force_pixels{};
};

struct SubmitStats {
    std::uint64_t none{}, state_commits{}, pixel_commits{}, failures{};
};

// A single xdg-shell toplevel. It owns Wayland objects and temporary SHM
// buffers; the DSL and renderer do not depend on these implementation types.
class WaylandWindow {
public:
    WaylandWindow();
    ~WaylandWindow();
    WaylandWindow(const WaylandWindow &) = delete;
    WaylandWindow &operator=(const WaylandWindow &) = delete;

    bool Open(const std::string &socket_name, const std::string &app_id, const std::string &title,
              int preferred_width, int preferred_height);

    void SetEventHandler(std::function<void(const contracts::WindowEvent &)> handler)
    {
        event_handler_ = std::move(handler);
    }

    void SetPaintHandler(std::function<void(void *, int, int, int)> handler)
    {
        paint_handler_ = std::move(handler);
    }

    void SetPresentationHandler(std::function<void(const PixelPresentation &)> handler)
    {
        presentation_handler_ = std::move(handler);
    }

    // Prepare Pixels renders an uncommitted WSI buffer; only commit_pixels
    // swaps it, after the window requests its frame/presentation objects.
    // State and None never request those objects. Failed is terminal: callbacks
    // must release external WSI resources before returning failure, then the
    // window destroys its surface to discard pending server-side state.
    void SetSubmitHandlers(std::function<SubmitResult(const SubmitRequest &)> prepare,
                           std::function<bool()> commit_pixels,
                           std::function<void(SubmitResult)> on_submitted = {})
    {
        prepare_submit_ = std::move(prepare);
        commit_pixels_ = std::move(commit_pixels);
        on_submitted_ = std::move(on_submitted);
    }

    void RequestRedraw(bool deferred = false);
    void RequestUpdate(bool deferred = false);
    // External descriptors only wake this pump. Read/cancel of the Wayland
    // read intention always precedes returning their revents to the caller.
    bool Pump(int timeout_ms, std::span<pollfd> wake_fds = {});
    void RequestMaximize();
    void SetSurfaceEffects(std::span<const contracts::SurfaceEffectRegion> regions);
    void SetInputRegions(std::span<const contracts::SurfaceInputRegion> regions);
    void Close();

    bool IsConfigured() const
    {
        return configured_;
    }

    bool IsMapped() const
    {
        return mapped_;
    }

    bool IsCloseRequested() const
    {
        return close_requested_;
    }

    int ConfigureCount() const
    {
        return configure_count_;
    }

    bool HasPresentationFeedback() const
    {
        return presentation_ != nullptr;
    }

    int PresentationCount() const
    {
        return presentation_count_;
    }

    int DiscardedCount() const
    {
        return discarded_count_;
    }

    int FrameDoneCount() const
    {
        return frame_done_count_;
    }

    bool FrameCallbackPending() const
    {
        return frame_callback_ != nullptr;
    }

    bool SurfaceStatePending() const
    {
        return state_pending_;
    }

    SubmitStats GetSubmitStats() const
    {
        return submit_stats_;
    }

    PixelSubmissionId LastPixelSubmission() const noexcept
    {
        return last_pixel_submission_;
    }

    bool PresentationPending(PixelSubmissionId submission) const noexcept;

    int PointerEnterCount() const
    {
        return pointer_enter_count_;
    }

    int PointerButtonCount() const
    {
        return pointer_button_count_;
    }

    int KeyCount() const
    {
        return key_count_;
    }

    contracts::WindowMetrics Metrics() const
    {
        return metrics_;
    }

private:
    struct ShmBuffer;
    static void RegistryGlobal(void *, wl_registry *, std::uint32_t, const char *, std::uint32_t);
    static void EffectCapabilities(void *, prism_surface_effect_manager_v1 *, std::uint32_t);
    static void RegistryGlobalRemove(void *, wl_registry *, std::uint32_t);
    static void ShellPing(void *, xdg_wm_base *, std::uint32_t);
    static void SurfaceConfigure(void *, xdg_surface *, std::uint32_t);
    static void ToplevelConfigure(void *, xdg_toplevel *, std::int32_t, std::int32_t, wl_array *);
    static void ToplevelClose(void *, xdg_toplevel *);
    static void ToplevelConfigureBounds(void *, xdg_toplevel *, std::int32_t, std::int32_t);
    static void ToplevelCapabilities(void *, xdg_toplevel *, wl_array *);
    static void SeatCapabilities(void *, wl_seat *, std::uint32_t);
    static void SeatName(void *, wl_seat *, const char *);
    static void PointerEnter(void *, wl_pointer *, std::uint32_t, wl_surface *, wl_fixed_t,
                             wl_fixed_t);
    static void PointerLeave(void *, wl_pointer *, std::uint32_t, wl_surface *);
    static void PointerMotion(void *, wl_pointer *, std::uint32_t, wl_fixed_t, wl_fixed_t);
    static void PointerButton(void *, wl_pointer *, std::uint32_t, std::uint32_t, std::uint32_t,
                              std::uint32_t);
    static void PointerAxis(void *, wl_pointer *, std::uint32_t, std::uint32_t, wl_fixed_t);
    static void PointerFrame(void *, wl_pointer *);
    static void PointerAxisSource(void *, wl_pointer *, std::uint32_t);
    static void PointerAxisStop(void *, wl_pointer *, std::uint32_t, std::uint32_t);
    static void PointerAxisDiscrete(void *, wl_pointer *, std::uint32_t, std::int32_t);
    static void PointerAxisValue120(void *, wl_pointer *, std::uint32_t, std::int32_t);
    static void PointerAxisRelativeDirection(void *, wl_pointer *, std::uint32_t, std::uint32_t);
    static void KeyboardKeymap(void *, wl_keyboard *, std::uint32_t, int, std::uint32_t);
    static void KeyboardEnter(void *, wl_keyboard *, std::uint32_t, wl_surface *, wl_array *);
    static void KeyboardLeave(void *, wl_keyboard *, std::uint32_t, wl_surface *);
    static void KeyboardKey(void *, wl_keyboard *, std::uint32_t, std::uint32_t, std::uint32_t,
                            std::uint32_t);
    static void KeyboardModifiers(void *, wl_keyboard *, std::uint32_t, std::uint32_t,
                                  std::uint32_t, std::uint32_t, std::uint32_t);
    static void KeyboardRepeatInfo(void *, wl_keyboard *, std::int32_t, std::int32_t);
    static void FrameDone(void *, wl_callback *, std::uint32_t);
    static void BufferRelease(void *, wl_buffer *);

    static void PresentationClock(void *, wp_presentation *, std::uint32_t);
    static void PresentationOutput(void *, struct wp_presentation_feedback *, wl_output *);
    static void PresentationDone(void *, struct wp_presentation_feedback *, std::uint32_t,
                                 std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                 std::uint32_t, std::uint32_t);
    static void PresentationDiscarded(void *, struct wp_presentation_feedback *);
    struct wp_presentation_feedback *TrackPresentation(PixelSubmissionId);
    bool PresentationCapacityAvailable() const noexcept;
    void FinishPresentation(struct wp_presentation_feedback *, PresentationOutcome);
    void DispatchCompletedPresentations();
    void DeliverPresentation(PixelPresentation);
    void ConfirmPixelSubmission(PixelSubmissionId) noexcept;
    ShmBuffer *AcquireBuffer();
    void Emit(contracts::WindowEvent event);
    SubmitResult TrySubmit();
    SubmitResult CompleteSubmit(SubmitResult);
    int DispatchPending();
    void ReapBuffers();

    wp_presentation *presentation_{nullptr};

    struct PendingPresentation {
        struct wp_presentation_feedback *handle{};
        PixelSubmissionId submission{};
        bool committed{};
        std::optional<PresentationOutcome> completed;
    };

    std::array<PendingPresentation, 8> feedbacks_{};
    PixelSubmissionId last_pixel_submission_{};
    PixelSubmissionId frame_callback_submission_{};
    int presentation_count_{0};
    int discarded_count_{0};
    wl_display *display_{nullptr};
    wl_registry *registry_{nullptr};
    wl_compositor *compositor_{nullptr};
    wl_shm *shm_{nullptr};
    wl_seat *seat_{nullptr};
    std::uint32_t seat_global_name_{0};
    wl_pointer *pointer_{nullptr};
    wl_keyboard *keyboard_{nullptr};
    xdg_wm_base *shell_{nullptr};
    wl_surface *surface_{nullptr};
    prism_surface_effect_manager_v1 *effect_manager_{};
    prism_surface_effect_v1 *surface_effect_{};
    bool backdrop_supported_{};
    std::vector<contracts::SurfaceEffectRegion> sent_effects_;
    std::vector<contracts::SurfaceInputRegion> sent_input_;
    bool input_sent_{};
    xdg_surface *xdg_surface_{nullptr};
    xdg_toplevel *toplevel_{nullptr};
    wl_callback *frame_callback_{nullptr};
    std::vector<std::unique_ptr<ShmBuffer>> buffers_;
    std::function<void(const contracts::WindowEvent &)> event_handler_;
    std::function<void(void *, int, int, int)> paint_handler_;
    std::function<SubmitResult(const SubmitRequest &)> prepare_submit_;
    std::function<bool()> commit_pixels_;
    std::function<void(SubmitResult)> on_submitted_;
    std::function<void(const PixelPresentation &)> presentation_handler_;
    SubmitStats submit_stats_{};
    contracts::WindowMetrics metrics_{};
    contracts::LogicalPoint pointer_position_{};
    int preferred_width_{640};
    int preferred_height_{400};
    int pending_width_{0};
    int pending_height_{0};
    int configure_count_{0};
    int frame_done_count_{0};
    int pointer_enter_count_{0};
    int pointer_button_count_{0};
    int key_count_{0};
    bool configured_{false};
    bool mapped_{false};
    bool update_requested_{false};
    bool force_pixels_{false};
    bool state_pending_{false};
    bool failed_{false};
    bool dispatching_{false};
    bool close_requested_{false};
};

} // namespace prism::platform
