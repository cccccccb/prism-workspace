#pragma once
#include "client_application_install_p.hpp"
#include "prism/platform/wayland_egl_surface.hpp"
#include "prism/platform/wayland_window.hpp"
#include "prism/render_skia/gles_renderer.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/buffer_damage.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/frame_packet.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/runtime/task_scheduler.hpp"
#include "prism/sdk/client_application.hpp"
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace prism::sdk {
class FirstCallTimer {
public:
    FirstCallTimer(std::uint64_t &duration, bool &sampled) : duration_(duration), record_(!sampled)
    {
        if (record_) {
            sampled = true;
            start_ = std::chrono::steady_clock::now();
        }
    }

    ~FirstCallTimer()
    {
        if (record_) {
            duration_ =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::steady_clock::now() - start_)
                                               .count());
        }
    }

    FirstCallTimer(const FirstCallTimer &) = delete;
    FirstCallTimer &operator=(const FirstCallTimer &) = delete;

private:
    std::uint64_t &duration_;
    bool record_;
    std::chrono::steady_clock::time_point start_{};
};

struct ClientApplication::Impl {
    explicit Impl(ClientConfig value)
        : config(std::move(value)), commands(config.font_path),
          resources(render_skia::RasterRenderer::InspectPng,
                    render_skia::RasterRenderer::DecodePngBounded, config.task_scheduler)
    {
        if (!config.install_limits.nodes_per_turn || !config.install_limits.images_per_turn ||
            !config.install_limits.upload_bytes_per_turn ||
            config.install_limits.cpu_per_turn.count() <= 0) {
            throw std::invalid_argument("UI installation limits must be positive");
        }
    }

    ~Impl();

    bool InstallScene(runtime::UiLoadId load, const runtime::PreparedComponent &prepared,
                      runtime::LoadDiagnostic *diagnostic);
    void ReleaseUnusedImages(const std::set<std::uint64_t> &keep);
    void ClearPreloadedImages();
    void DiscardInstall(runtime::UiInstallState state);
    void DropImage(contracts::ResourceId id);
    bool RegisterImage(contracts::ResourceId id);
    void QueueImageUpload(contracts::ResourceId id);
    bool EnsureRenderer(int width, int height);
    bool AdvanceImageUploads();
    void EnsureUiWorkBudget();
    void ResetUiWorkBudget() noexcept;
    void RecordUiWorkBudget() noexcept;

    struct PumpTurnGuard {
        Impl &app;

        ~PumpTurnGuard()
        {
            if (!app.ui_work_turn_explicit) {
                app.RecordUiWorkBudget();
                app.ResetUiWorkBudget();
                app.install_advanced_since_pump = false;
            }
        }
    };

    bool ImagesUploaded() const;
    void CommitScene(runtime::UiLoadId load, std::unique_ptr<runtime::Scene> next,
                     const std::set<std::uint64_t> &images);
    bool OpenWindow(runtime::LoadDiagnostic *diagnostic, const runtime::ComponentSource &source);
    bool CommitInstall(const runtime::BindingValues &bindings, runtime::LoadDiagnostic *diagnostic);
    contracts::ResourceId RequestImage(std::set<std::uint64_t> &images, std::string_view uri);
    runtime::ShapedText ShapeText(std::string_view text, double size);
    void HandleWindowEvent(const contracts::WindowEvent &event);
    std::shared_ptr<const runtime::FramePacket>
    CaptureFramePacket(bool pixels, contracts::BufferSize size, double scale, int configure_count);
    platform::SubmitResult PrepareSubmit(const platform::SubmitRequest &request);
    bool CommitPixels();
    void Submitted(platform::SubmitResult result);
    void HandlePresentation(const platform::PixelPresentation &event);
    bool PollResources();
    void CloseGpu();
    void FailFrontend();
    std::set<std::uint64_t> scene_images;
    std::set<std::uint64_t> preloaded_images;
    runtime::UiLoadId preloaded_ui{};
    std::set<std::uint64_t> registered_images;
    std::deque<contracts::ResourceId> upload_queue;
    std::set<std::uint64_t> queued_uploads;
    std::unique_ptr<StagedUiInstall> install;
    runtime::UiInstallState install_state{runtime::UiInstallState::Idle};
    runtime::UiInstallStats install_stats;
    runtime::BindingValues binding_values;
    bool install_advanced_since_pump{};
    bool ui_work_turn_explicit{};
    bool ui_work_turn_started{};
    std::chrono::steady_clock::time_point owner_turn_deadline{};
    std::size_t owner_turn_uploads{};
    std::uint64_t owner_turn_upload_bytes{};
    std::size_t owner_turn_nodes{}, owner_turn_requests{}, owner_turn_registrations{},
        owner_turn_image_ready{};
    runtime::UiLoadState ui_load;
    runtime::UiLoadId installed_ui{};
    UiPresentationTracker ui_presentation;
    ClientConfig config;
    render_skia::RasterRenderer commands;
    runtime::ImageResources resources;
    std::unique_ptr<runtime::Scene> scene;
    std::optional<contracts::ThemeSnapshot> theme;
    std::shared_ptr<const contracts::DisplayList> last_list;
    std::shared_ptr<const runtime::FramePacket> committed_frame, prepared_frame;
    runtime::BufferDamageHistory damage_history;
    std::optional<runtime::BufferDamagePlan> prepared_damage;
    std::uint64_t prepared_content_area{};
    platform::WaylandWindow window;
    platform::WaylandEglSurface egl;
    std::unique_ptr<render_skia::GlesRenderer> renderer;
    std::function<void(std::string_view)> on_action;
    std::function<void(runtime::UiLoadId)> on_ui_submitted;
    std::string gl_renderer;
    int requested_images{0};
    int loaded_images{0};
    int presented{0};
    ClientRenderStats render_stats{}; // Scene fields retain retired UI counters.
    ClientStartupStats startup_stats{};
    bool egl_init_sampled{}, ganesh_init_sampled{}, submit_build_sampled{};
    bool render_sampled{}, swap_sampled{};
    bool failed{false};
    bool opened_once{false};
    bool closed{false};
};

inline void AddSceneStats(ClientRenderStats &total, const runtime::SceneRenderStats &scene)
{
    total.scene_build_attempts += scene.build_calls;
    total.scene_builds += scene.builds;
    total.scene_layouts += scene.layouts;
}
} // namespace prism::sdk
