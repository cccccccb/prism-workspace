#pragma once
#include "prism/contracts/theme.hpp"
#include "prism/runtime/prepared_component.hpp"
#include "prism/runtime/prepared_regions.hpp"
#include "prism/runtime/property.hpp"
#include "prism/runtime/ui_install.hpp"
#include "prism/runtime/ui_load.hpp"
#include "prism/sdk/ui_presentation.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>

namespace prism::runtime {
class TaskScheduler;
enum class ImageState;
} // namespace prism::runtime

namespace prism::sdk {

std::optional<std::string> LoadUiSource(std::string_view installed_name,
                                        std::string_view source_path);

struct ClientConfig {
    std::string socket;
    std::string app_id;
    std::string title;
    std::string font_path;
    int width{640};
    int height{400};
    std::string assets_root{}; // If set, image URIs resolve strictly beneath this root.
    // Optional backend resource policy. No override uses the renderer default.
    std::optional<std::size_t> gpu_resource_cache_bytes;
    // Auto uses buffer age to repair only changed pixels. Disabling it keeps
    // the same renderer and content damage contract, with full pixel repair.
    bool partial_rendering{true};
    std::shared_ptr<runtime::TaskScheduler> task_scheduler{};
    runtime::UiInstallLimits install_limits{};
};

// Cumulative observations for this ClientApplication, including UI replacement.
// A Build call can produce no DisplayList. GPU/swap attempts count calls to the
// corresponding backend methods; successes count their true return values.
// Detached theme preflight candidates are excluded from submitted scene work.
struct ClientRenderStats {
    std::uint64_t scene_build_attempts{}, scene_builds{}, scene_layouts{};
    std::uint64_t gpu_render_attempts{}, gpu_render_successes{};
    std::uint64_t swap_attempts{}, swap_successes{};
    std::uint64_t frame_callbacks_done{};
    std::uint64_t surface_state_commits{}, surface_pixel_commits{};
    std::uint64_t surface_submission_failures{}, surface_noops{};
    std::uint64_t full_pixel_repairs{}, partial_pixel_repairs{}, empty_pixel_repairs{};
    std::uint64_t pixel_repair_pixels{}, content_damage_pixels{};
    std::uint64_t damage_history_commits{}, buffer_age_queries{}, unknown_buffer_ages{};
    int last_buffer_age{-1}; // -1 unavailable, 0 contents undefined.
    bool buffer_age_supported{}, swap_damage_supported{}, partial_update_supported{};
};

// First actual backend/submit-path calls, measured with a monotonic wall clock.
// A zero duration may be a sub-microsecond call; it does not mean unexecuted.
// Failed calls are measured too. Render/Swap do not imply GPU completion.
struct ClientStartupStats {
    std::uint64_t egl_init_us{}, ganesh_init_us{}, first_submit_build_us{};
    std::uint64_t first_render_us{}, first_swap_us{};
};

// Client-owned DSL scene, resources, Skia GLES renderer and Wayland window.
// All methods except construction/destruction run on the Wayland thread.
class ClientApplication {
public:
    explicit ClientApplication(ClientConfig config);
    ~ClientApplication();
    ClientApplication(const ClientApplication &) = delete;
    ClientApplication &operator=(const ClientApplication &) = delete;

    // Worker-only construction initializes font and resource handles; no Wayland/GPU yet.
    bool FrontendReady() const;
    bool ConfigureWindow(ClientConfig config);   // Before Open only, common font unchanged.
    bool ReplaceUi(std::string_view dsl_source); // Keeps the same surface and EGL context.
    bool Open(std::string_view dsl_source);

    // Issue on the owner thread before dispatching pure PrepareComponent work.
    // A new load or cancellation invalidates earlier results without changing the live UI.
    runtime::UiLoadId BeginUiLoad();
    void CancelUiLoad();
    UiPresentationState GetUiPresentation(runtime::UiLoadId load) const noexcept;
    // Every successful pixel submission, after its UI identity is recorded.
    // Observers must return promptly and leave installation to the owner pump.
    void OnUiSubmitted(std::function<void(runtime::UiLoadId)> callback);
    bool OpenPrepared(runtime::UiLoadId load, const runtime::PreparedComponent &prepared,
                      runtime::LoadDiagnostic *diagnostic = nullptr);
    bool ReplaceUiPrepared(runtime::UiLoadId load, const runtime::PreparedComponent &prepared,
                           runtime::LoadDiagnostic *diagnostic = nullptr);
    bool StartPreparedInstall(runtime::UiLoadId load, const runtime::PreparedComponent &prepared,
                              runtime::LoadDiagnostic *diagnostic = nullptr);
    bool StartRegionInstall(runtime::UiLoadId load,
                            std::span<const runtime::PreparedRegion> regions,
                            runtime::LoadDiagnostic *diagnostic = nullptr);
    runtime::UiInstallState AdvanceUiInstall(const runtime::BindingValues &bindings,
                                             runtime::LoadDiagnostic *diagnostic = nullptr);
    bool UiInstallPending() const noexcept;
    bool UiInstallNeedsWork() const noexcept;
    // Owner-only work scope. Nested Begin throws; an unbalanced End returns
    // false. All installs/uploads in a scope share a lazily started allowance.
    void BeginUiWorkTurn();
    bool EndUiWorkTurn() noexcept;
    runtime::UiInstallStats GetUiInstallStats() const noexcept;
    // Stage required images while keeping the current Scene. Resource parsing
    // and decode share the Host scheduler; registration stays on this owner.
    bool PreloadImages(runtime::UiLoadId, const runtime::PreparedComponent &,
                       runtime::LoadDiagnostic *diagnostic = nullptr);
    runtime::ImageState PreloadedImageState(runtime::UiLoadId) const;
    int ResourceCompletionFd() const noexcept;
    bool PollImageResources();
    bool Pump(int timeout_ms, std::span<pollfd> wake_fds = {});
    bool SetSlot(std::string_view name, std::string value);
    bool SetBinding(std::string_view name, runtime::PropertyValue value);
    bool ApplyTheme(const contracts::ThemeSnapshot &, std::string *diagnostic = nullptr);
    std::uint64_t ThemeGeneration() const;
    void OnAction(std::function<void(std::string_view)> callback);
    bool IsCloseRequested() const;
    bool IsMapped() const;
    int ConfigureCount() const;
    int FrameDoneCount() const;
    bool FrameCallbackPending() const;
    int PresentedCount() const; // Compatibility: swap submissions, not actual presentation.
    bool HasPresentationFeedback() const;
    int PresentationCount() const;
    std::uint64_t WaitDurationNs() const noexcept;
    ClientRenderStats GetRenderStats() const;
    ClientStartupStats GetStartupStats() const noexcept;
    int RequestedImageCount() const;
    int LoadedImageCount() const;
    std::string GlRenderer() const;
    void Close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace prism::sdk
