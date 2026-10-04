#pragma once
#include "client_application_install_p.hpp"
#include "client_render_owner_p.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/frame_packet.hpp"
#include "prism/runtime/png_codec.hpp"
#include "prism/runtime/pollable_queue.hpp"
#include "prism/runtime/render_command.hpp"
#include "prism/runtime/render_event.hpp"
#include "prism/runtime/render_worker_lifecycle.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/runtime/task_scheduler.hpp"
#include "prism/runtime/terminal_signal.hpp"
#include "prism/runtime/text_shaper.hpp"
#include "prism/sdk/client_application.hpp"
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace prism::sdk {
// One worker incarnation owns one set of pollable queues and a one-shot
// terminal/lifecycle signal. A failed initial Open can replace the whole
// bridge only after the worker has acknowledged Close and been joined.
struct ClientRenderBridge {
    runtime::PollableQueue<runtime::RenderCommand> render_commands{8192};
    runtime::PollableQueue<runtime::RenderEvent> render_events{8192};
    runtime::TerminalSignal terminal;
    runtime::RenderWorkerLifecycle worker_lifecycle{terminal};
};

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
        : config(std::move(value)), shaper(config.font_path), commands(config.font_path),
          resources(runtime::InspectPng, runtime::DecodePngBounded, config.task_scheduler)
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
    bool OpenRenderWorker(std::string *failure, std::string *detail);
    void CloseRenderWorker() noexcept;
    bool ResetRenderBridge() noexcept;
    void QueueStopRenderWorker() noexcept;
    void QueueRenderInstallUi(runtime::UiLoadId load);
    void QueueRenderInvalidate(runtime::UiLoadId load);
    void InvalidateQueuedFrame();
    void QueueRenderUpdate(bool deferred = false);
    void QueueRenderRedraw(bool deferred = false);
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

    void CommitScene(runtime::UiLoadId load, std::unique_ptr<runtime::Scene> next,
                     const std::set<std::uint64_t> &images);
    bool OpenWindow(runtime::LoadDiagnostic *diagnostic, const runtime::ComponentSource &source);
    bool CommitInstall(const runtime::BindingValues &bindings, runtime::LoadDiagnostic *diagnostic);
    contracts::ResourceId RequestImage(std::set<std::uint64_t> &images, std::string_view uri);
    runtime::ShapedText ShapeText(std::string_view text, double size);
    void ProcessRenderEvents(bool deliver);
    void CollectGestureEvents();
    void DeliverGestureEvents();
    void ApplyRenderStatus(const runtime::RenderStatusEvent &status);
    void HandleWindowEvent(const contracts::WindowEvent &event,
                           const std::shared_ptr<const runtime::InputSnapshot> &input);
    void HandleSubmitted(const runtime::SubmittedFrameEvent &event);
    void PublishFramePacket();
    std::shared_ptr<const runtime::FramePacket>
    CaptureFramePacket(bool pixels, contracts::BufferSize size, double scale, int configure_count);
    void HandlePresentation(const platform::PixelPresentation &event);
    void HandleFrameOpportunity(const runtime::FrameOpportunityEvent &event);
    void SyncAnimationSampling();
    void AdvanceAnimationDeadline();
    int AnimationTimeoutMs(int timeout_ms) const noexcept;
    bool HasUnsubmittedPixels() const noexcept;
    bool PollResources();
    void FailFrontend();
    std::set<std::uint64_t> scene_images;
    std::set<std::uint64_t> preloaded_images;
    runtime::UiLoadId preloaded_ui{};
    std::set<std::uint64_t> registered_images;
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
    runtime::TextShaper shaper;
    render_skia::RasterRenderer commands;
    runtime::ImageResources resources;
    std::unique_ptr<ClientRenderBridge> bridge{std::make_unique<ClientRenderBridge>()};
    std::optional<runtime::RenderWorkerGeneration> worker_generation;
    std::unique_ptr<ClientRenderOwner> render_owner;
    ClientPlatformStatus platform_status{};
    contracts::WindowMetrics ui_metrics{};
    int ui_configure_count{};
    std::map<std::uint64_t, std::uint64_t> uploaded_image_versions;
    std::set<std::pair<std::uint64_t, std::uint64_t>> pending_release_versions;
    std::unique_ptr<runtime::Scene> scene;
    std::optional<contracts::ThemeSnapshot> theme;
    std::shared_ptr<const contracts::DisplayList> last_list;
    std::shared_ptr<const std::vector<runtime::ImageVersion>> last_image_uses;
    std::shared_ptr<const runtime::FramePacket> queued_frame, ui_submitted_frame;
    std::uint64_t next_frame_sequence{};
    std::optional<std::uint64_t> animation_deadline_ns;
    std::optional<std::uint64_t> pending_animation_finish_sequence;
    bool animation_worker_active{};
    std::uint64_t last_processed_window_sequence{};
    bool force_frame_capture{};
    std::function<bool()> on_close_requested;
    std::function<void(std::string_view)> on_action;
    std::function<void(std::string_view, std::string_view)> on_text_edit;
    std::function<void(const contracts::GestureEvent &)> on_gesture;

    struct PendingGesture {
        runtime::UiLoadId ui;
        contracts::GestureEvent event;
    };

    std::deque<PendingGesture> pending_gestures;
    std::set<std::uint64_t> delivered_gestures;
    bool delivering_gestures{};
    std::function<void(runtime::UiLoadId)> on_ui_submitted;
    std::string gl_renderer;
    int requested_images{0};
    int loaded_images{0};
    int presented{0};
    ClientRenderStats render_stats{}; // Scene fields retain retired UI counters.
    ClientStartupStats startup_stats{};
    bool submit_build_sampled{};
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
