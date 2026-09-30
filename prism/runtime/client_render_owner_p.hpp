#pragma once

#include "prism/platform/wayland_egl_surface.hpp"
#include "prism/platform/wayland_window.hpp"
#include "prism/render_skia/gles_renderer.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/buffer_damage.hpp"
#include "prism/runtime/pollable_queue.hpp"
#include "prism/runtime/render_command.hpp"
#include "prism/runtime/render_event.hpp"
#include "prism/runtime/render_worker_lifecycle.hpp"
#include "prism/sdk/client_application.hpp"

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace prism::sdk {

// The only owner of this client's Wayland proxies, EGL context and Ganesh
// resources. Its public methods never inspect those objects from the UI thread.
// The UI must request Close and wait for CompleteClose before destroying the
// queues, terminal signal or this owner.
class ClientRenderOwner {
public:
    using Snapshot = runtime::RenderStatusEvent;

    ClientRenderOwner(ClientConfig config, runtime::PollableQueue<runtime::RenderCommand> &commands,
                      runtime::PollableQueue<runtime::RenderEvent> &events,
                      runtime::TerminalSignal &terminal, runtime::RenderWorkerLifecycle &lifecycle);
    ~ClientRenderOwner();

    ClientRenderOwner(const ClientRenderOwner &) = delete;
    ClientRenderOwner &operator=(const ClientRenderOwner &) = delete;

    bool Start(runtime::RenderWorkerGeneration generation);
    void Join();
    Snapshot ReadSnapshot() const;

private:
    struct RenderImage {
        runtime::ImageVersion version;
        runtime::ImageLease pixels;
    };

    void Run(runtime::RenderWorkerGeneration generation) noexcept;
    bool OpenWindow();
    void CloseRenderState() noexcept;
    void CloseGpu() noexcept;
    void PublishStatus(bool enqueue = true);
    bool QueueEvent(runtime::RenderEvent event) noexcept;
    void QueueWindowEvent(const contracts::WindowEvent &event) noexcept;
    void QueuePresentationEvent(const platform::PixelPresentation &event) noexcept;

    bool DrainCommands();
    void SetAnimationSampling(runtime::SetAnimationSamplingCommand command);
    void AnswerFrameOpportunity(runtime::AnswerFrameOpportunityCommand command);
    bool IssueFrameOpportunity();
    void ResetFrameOpportunity() noexcept;
    bool RegisterImage(runtime::RegisterImageCommand command);
    bool ReleaseImage(runtime::ReleaseImageCommand command);
    bool AdvanceImageUploads();
    bool EnsureRenderer(int width, int height);
    bool ImagesUploaded(const runtime::FramePacket &frame) const;
    void QueueImageUpload(contracts::ResourceId id);

    platform::SubmitResult PrepareSubmit(const platform::SubmitRequest &request);
    bool CommitPixels();
    void Submitted(platform::SubmitResult result) noexcept;

    ClientConfig config_;
    runtime::PollableQueue<runtime::RenderCommand> &commands_;
    runtime::PollableQueue<runtime::RenderEvent> &events_;
    runtime::TerminalSignal &terminal_;
    runtime::RenderWorkerLifecycle &lifecycle_;
    mutable std::mutex snapshot_mutex_;
    Snapshot snapshot_{};
    std::optional<Snapshot> last_published_status_;
    std::thread worker_;

    // All fields below are touched only by worker_ after Start.
    std::unique_ptr<render_skia::RasterRenderer> damage_commands_;
    platform::WaylandWindow window_;
    platform::WaylandEglSurface egl_;
    std::unique_ptr<render_skia::GlesRenderer> renderer_;
    std::map<std::uint64_t, RenderImage> render_images_;
    std::deque<contracts::ResourceId> upload_queue_;
    std::set<std::uint64_t> queued_uploads_;
    runtime::UiLoadId installed_ui_{};
    runtime::UiLoadId input_ui_{};
    std::shared_ptr<const runtime::InputSnapshot> input_snapshot_;
    runtime::RenderWorkerGeneration worker_generation_{};
    std::optional<runtime::FrameOpportunityEvent> frame_opportunity_;
    std::shared_ptr<const runtime::FramePacket> approved_frame_;
    std::uint64_t next_frame_opportunity_id_{};
    std::uint64_t opportunity_candidate_sequence_{};
    bool animation_sampling_active_{};
    bool spontaneous_animation_frame_allowed_{};
    std::shared_ptr<const runtime::FramePacket> render_frame_, committed_frame_, prepared_frame_;
    std::uint64_t committed_damage_resource_epoch_{};
    runtime::BufferDamageHistory damage_history_;
    std::optional<runtime::BufferDamagePlan> prepared_damage_;
    std::uint64_t prepared_content_area_{};
    std::uint64_t issued_event_sequence_{}, processed_event_sequence_{};
    runtime::RenderBackendStats backend_stats_{};
    runtime::RenderStartupStats startup_stats_{};
    std::string gl_renderer_;
    int presented_{};
    bool egl_init_sampled_{}, ganesh_init_sampled_{}, render_sampled_{}, swap_sampled_{};
    bool failed_{};
};

} // namespace prism::sdk
