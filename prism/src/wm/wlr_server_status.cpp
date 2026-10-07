#include "prism/ipc/wm_messages.hpp"
#include "wlr_server_internal.hpp"

namespace prism::wm {
namespace {
ipc::TimingMessage ToTiming(const TimingSamples &samples)
{
    const auto summary = samples.Summarize();
    return {summary.count, summary.mean, summary.p50, summary.p95, summary.p99, summary.maximum};
}

ipc::SchedulingMessage ToScheduling(const FrameWorkCounters &counters)
{
    ipc::SchedulingMessage message;
    message.frame_events = counters.frame_events;
    message.idle_skips = counters.idle_skips;
    message.scene_commit_calls = counters.scene_commit_calls;
    message.scene_commit_noops = counters.scene_commit_noops;
    message.output_commits = counters.output_commits;
    message.output_buffer_commits = counters.output_buffer_commits;
    message.frame_done_dispatches = counters.frame_done_dispatches;
    message.needs_frame_events = counters.needs_frame_events;
    message.damage_events = counters.damage_events;
    message.surface_commits = counters.surface_commits;
    message.surface_buffer_commits = counters.surface_buffer_commits;
    message.surface_callback_commits = counters.surface_callback_commits;
    message.surface_nonvisual_commits = counters.surface_nonvisual_commits;
    message.schedule_requests = {counters.layout_requests, counters.effects_requests,
                                 counters.mode_requests};
    return message;
}

ipc::EffectsWorkMessage ToEffects(const SurfaceEffects::WorkCounters &counters)
{
    ipc::EffectsWorkMessage message;
    message.update_calls = counters.update_calls;
    message.skipped_updates = counters.skipped_updates;
    message.unsupported_updates = counters.unsupported_updates;
    message.dirty_transitions = counters.dirty_transitions;
    message.wake_notifications = counters.wake_notifications;
    message.regions_checked = counters.regions_checked;
    message.cache_hits = counters.cache_hits;
    message.cache_misses = counters.cache_misses;
    message.capture_pass_attempts = counters.capture_pass_attempts;
    message.capture_passes = counters.capture_passes;
    message.blur_pass_attempts = counters.blur_pass_attempts;
    message.blur_passes = counters.blur_passes;
    message.material_pass_attempts = counters.material_pass_attempts;
    message.material_passes = counters.material_passes;
    message.allocation_attempts = counters.allocation_attempts;
    message.allocated_buffers = counters.allocated_buffers;
    message.allocation_failures = counters.allocation_failures;
    message.rendered_regions = counters.rendered_regions;
    message.rendered_pixels = counters.rendered_pixels;
    message.failed_regions = counters.failed_regions;
    message.invalid_regions = counters.invalid_regions;
    message.removed_regions = counters.removed_regions;
    message.scene_reorders = counters.scene_reorders;
    message.dependency_leaves_checked = counters.dependency_leaves_checked;
    message.dependency_leaves_included = counters.dependency_leaves_included;
    message.dependency_leaves_skipped = counters.dependency_leaves_skipped;
    message.content_revisions = counters.content_revisions;
    message.metadata_commits = counters.metadata_commits;
    message.damage_history_fallbacks = counters.damage_history_fallbacks;
    message.partial_damage_cache_hits = counters.partial_damage_cache_hits;
    message.capture_nodes = counters.capture_nodes;
    message.mask_builds = counters.mask_builds;
    message.mask_cache_hits = counters.mask_cache_hits;
    message.mask_failures = counters.mask_failures;
    return message;
}
} // namespace

std::string WlrServer::IpcStatus(const std::string &, const std::vector<std::string> &)
{
    ipc::StatusReply reply;
    reply.fps = ipc::WireFixedValue(current_fps_, 1);
    reply.frame_count = frame_count_;
    reply.outputs_count = outputs_.size();
    reply.windows_count = compositor_ ? compositor_->GetWindows().size() : 0;
    reply.mission_control = compositor_ && compositor_->IsInMissionControl();
    reply.debug_hud = compositor_ && compositor_->IsDebugHudEnabled();
    if (theme_snapshot_) {
        reply.theme = {theme_snapshot_->id, theme_snapshot_->generation,
                       theme_snapshot_->color_scheme};
    }
    reply.wayland_socket = socket_name_;
    reply.ipc_socket = ipc_server_->GetSocketPath();

    auto &performance = reply.performance;
    performance.frame_cpu = ToTiming(frame_cpu_);
    performance.effects_cpu = ToTiming(effects_cpu_);
    performance.commit_cpu = ToTiming(commit_cpu_);
    performance.commit_successes = commit_successes_;
    performance.commit_failures = commit_failures_;
    performance.pointer_events = pointer_events_;
    performance.pointer_event_age = ToTiming(pointer_event_age_);
    for (const auto &output : outputs_) {
        performance.presentation.push_back({output->wlr_output->name, output->presented_count,
                                            output->discarded_count,
                                            ToTiming(output->present_intervals)});
    }
    performance.scheduling = ToScheduling(frame_work_);
    performance.effects_work =
        ToEffects(surface_effects_ ? surface_effects_->Counters() : SurfaceEffects::WorkCounters{});
    return nlohmann::json(reply).dump();
}
} // namespace prism::wm
