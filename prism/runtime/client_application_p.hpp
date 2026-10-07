#pragma once
#include "client_application_install_p.hpp"
#include "client_render_owner_p.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/control_value_delivery.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/frame_packet.hpp"
#include "prism/runtime/owner_feedback_panel.hpp"
#include "prism/runtime/owner_feedback_session.hpp"
#include "prism/runtime/owner_file_panel.hpp"
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
#include <thread>
#include <tuple>
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

    bool AcceptClose();

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
    void DeliverInteractionEvents();
    void CollectControlEvents();
    runtime::ValueCancelReason ValidateControlDelivery(runtime::UiLoadId ui,
                                                       const runtime::ControlEdit &edit) const;
    void ApplyRenderStatus(const runtime::RenderStatusEvent &status);
    void HandleWindowEvent(const contracts::WindowEvent &event,
                           const std::shared_ptr<const runtime::InputSnapshot> &input);
    void HandleSubmitted(const runtime::SubmittedFrameEvent &event);
    void HandlePopupConfigure(const runtime::PopupConfigureEvent &event);
    void HandlePopupClosed(const runtime::PopupClosedEvent &event);
    void HandlePopupInput(const runtime::PopupInputEvent &event);
    void HandlePopupSubmitted(const runtime::PopupSubmittedEvent &event);
    void CapturePopupFrame(runtime::FramePacket &packet);
    void RejectPopup(const runtime::PopupSurfaceIdentity &identity);
    void ResetPopupSurface();
    void CompleteInteractionResult(const runtime::InteractionResult &result,
                                   runtime::UiLoadId input_ui);
    void ReconcileOwnerTask();
    void CancelCurrentOwnerTask(runtime::TaskCancelReason reason);
    void RevokeOwnerTaskScope();
    void AdoptOwnerTaskInput(const std::shared_ptr<const runtime::InputSnapshot> &input,
                             runtime::UiLoadId ui);
    void RetireOwnerTasks(runtime::TaskCancelReason reason);
    void PublishOwnerTaskChange();
    void UpdateOwnerConfirmationText();
    void ClearOwnerConfirmation();
    void ClearOwnerFilePanel();
    void UpdateOwnerFilePanelText();
    void ClearOwnerFeedback(bool clear_pending = false);
    void RetireOwnerFeedback();
    void UpdateOwnerFeedbackText();
    void ReconcileOwnerFeedback();
    void AdoptOwnerFeedbackInput(const std::shared_ptr<const runtime::InputSnapshot> &input,
                                 runtime::UiLoadId ui);
    void ObserveOwnerFeedbackEvent(const contracts::WindowEvent &event);
    bool HandleOwnerFeedbackAction(const runtime::Activation &activation);
    bool OwnerFeedbackPaused() const;
    int FeedbackTimeoutMs(int timeout_ms) const noexcept;
    void ReconcileTooltip();
    int TooltipTimeoutMs(int timeout_ms) const noexcept;
    runtime::Blueprint ComposeOwnerPanels(runtime::Blueprint blueprint) const;
    runtime::BindingValues OwnerPanelDefaults() const;
    std::shared_ptr<const std::vector<runtime::ImageVersion>>
    CollectImageUses(const contracts::DisplayList &list) const;
    void PublishFramePacket();
    std::shared_ptr<const runtime::FramePacket>
    CaptureFramePacket(bool pixels, contracts::BufferSize size, double scale, int configure_count);
    void HandlePresentation(const platform::PixelPresentation &event);
    void HandleFrameOpportunity(const runtime::FrameOpportunityEvent &event);
    void SyncAnimationSampling();
    void AdvanceAnimationDeadline();
    int AnimationTimeoutMs(int timeout_ms) const noexcept;
    bool HasUnsubmittedPixels() const noexcept;
    bool HasUnsubmittedPopupPixels() const noexcept;
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

    struct OwnerTaskScope {
        runtime::TaskIdentity identity;
        runtime::UiLoadId ui;
        std::uint64_t token{};
    };

    std::unique_ptr<runtime::TaskSession> owner_tasks;
    std::optional<OwnerTaskScope> owner_task_scope;
    bool owner_tasks_retired{};
    std::optional<runtime::PreparedComponent> owner_task_panel_template;
    std::optional<runtime::PreparedComponent> owner_file_panel_template;
    std::optional<runtime::OwnerFilePanelView> owner_file_view;
    std::optional<contracts::OwnerTaskKind> owner_file_kind;
    runtime::UiLoadId owner_file_ui{};
    std::uint64_t owner_file_generation{};
    std::optional<contracts::OwnerTaskRequest> owner_confirmation;
    runtime::BindingValues owner_task_bindings;
    std::uint64_t owner_confirmation_generation{};
    runtime::UiLoadId owner_confirmation_ui{};
    std::optional<runtime::PreparedComponent> owner_feedback_panel_template;
    std::optional<contracts::OwnerFeedbackRequest> owner_feedback;
    runtime::BindingValues owner_feedback_bindings;
    runtime::UiLoadId owner_feedback_ui{};
    std::uint64_t owner_feedback_generation{};
    runtime::OwnerFeedbackSession owner_feedback_session;
    std::optional<contracts::OwnerFeedbackAction> owner_feedback_action;
    bool owner_feedback_retired{}, owner_feedback_hidden{}, owner_feedback_wait_adoption{};
    bool owner_feedback_focus_known{}, owner_feedback_window_focused{true};
    std::set<std::uint64_t> owner_feedback_focused_seats;
    std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>, contracts::LogicalPoint>
        owner_feedback_pointers;
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
    std::shared_ptr<const runtime::FramePacket> ui_root_metadata_frame;
    std::optional<runtime::PopupConfigureEvent> popup_configuration;
    std::shared_ptr<const runtime::PopupFramePacket> ui_popup_submitted_frame;
    runtime::PopupSurfaceIdentity ui_popup_submitted_identity{};
    std::optional<runtime::PopupSurfaceIdentity> rejected_popup_identity;
    std::shared_ptr<const runtime::PopupSurfaceRequest> rejected_popup_request;
    std::uint64_t next_frame_sequence{};
    std::optional<std::uint64_t> animation_deadline_ns;
    std::optional<std::uint64_t> pending_animation_finish_sequence;
    bool animation_worker_active{};
    std::uint64_t last_processed_window_sequence{};
    bool force_frame_capture{};
    std::function<bool()> on_close_requested;
    const std::thread::id owner_thread{std::this_thread::get_id()};
    bool close_accept_queued{};
    std::function<void(std::string_view)> on_action;
    runtime::ControlValueDelivery control_delivery;
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
