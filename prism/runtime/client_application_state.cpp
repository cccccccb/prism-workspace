#include "client_application_p.hpp"

namespace prism::sdk {

void ClientApplication::Impl::ApplyRenderStatus(const runtime::RenderStatusEvent &status)
{
    const auto &platform = status.platform;
    if (platform.configure_count >= ui_configure_count) {
        platform_status = {
            .close_requested = platform.close_requested,
            .configured = platform.configured,
            .mapped = platform.mapped,
            .frame_callback_pending = platform.frame_callback_pending,
            .presentation_feedback = platform.presentation_feedback,
            .metrics = platform.metrics,
            .configure_count = platform.configure_count,
            .frame_done_count = platform.frame_done_count,
            .presentation_count = platform.presentation_count,
            .wait_duration_ns = platform.wait_duration_ns,
            .surface_state_commits = platform.surface_state_commits,
            .surface_pixel_commits = platform.surface_pixel_commits,
            .surface_submission_failures = platform.surface_submission_failures,
            .surface_noops = platform.surface_noops,
        };
    }

    const auto &backend = status.backend;
    render_stats.gpu_render_attempts = backend.gpu_render_attempts;
    render_stats.gpu_render_successes = backend.gpu_render_successes;
    render_stats.swap_attempts = backend.swap_attempts;
    render_stats.swap_successes = backend.swap_successes;
    render_stats.full_pixel_repairs = backend.full_pixel_repairs;
    render_stats.partial_pixel_repairs = backend.partial_pixel_repairs;
    render_stats.empty_pixel_repairs = backend.empty_pixel_repairs;
    render_stats.pixel_repair_pixels = backend.pixel_repair_pixels;
    render_stats.content_damage_pixels = backend.content_damage_pixels;
    render_stats.damage_history_commits = backend.damage_history_commits;
    render_stats.buffer_age_queries = backend.buffer_age_queries;
    render_stats.unknown_buffer_ages = backend.unknown_buffer_ages;
    render_stats.last_buffer_age = backend.last_buffer_age;
    render_stats.buffer_age_supported = backend.buffer_age_supported;
    render_stats.swap_damage_supported = backend.swap_damage_supported;
    render_stats.partial_update_supported = backend.partial_update_supported;

    install_stats.image_uploads = backend.image_uploads;
    install_stats.upload_bytes = backend.upload_bytes;
    install_stats.oversized_uploads = backend.oversized_uploads;
    startup_stats.egl_init_us = status.startup.egl_init_us;
    startup_stats.ganesh_init_us = status.startup.ganesh_init_us;
    startup_stats.first_render_us = status.startup.first_render_us;
    startup_stats.first_swap_us = status.startup.first_swap_us;
    gl_renderer = status.gl_renderer;
    presented = status.presented;
}

bool ClientApplication::IsCloseRequested() const
{
    return impl_->platform_status.close_requested;
}

bool ClientApplication::IsMapped() const
{
    return impl_->platform_status.mapped;
}

int ClientApplication::ConfigureCount() const
{
    return impl_->platform_status.configure_count;
}

int ClientApplication::FrameDoneCount() const
{
    return impl_->platform_status.frame_done_count;
}

bool ClientApplication::FrameCallbackPending() const
{
    return impl_->platform_status.frame_callback_pending;
}

int ClientApplication::PresentedCount() const
{
    return impl_->presented;
}

bool ClientApplication::HasPresentationFeedback() const
{
    return impl_->platform_status.presentation_feedback;
}

int ClientApplication::PresentationCount() const
{
    return impl_->platform_status.presentation_count;
}

std::uint64_t ClientApplication::WaitDurationNs() const noexcept
{
    return impl_->platform_status.wait_duration_ns;
}

ClientPlatformStatus ClientApplication::GetPlatformStatus() const noexcept
{
    return impl_->platform_status;
}

ClientRenderStats ClientApplication::GetRenderStats() const
{
    auto stats = impl_->render_stats;
    if (impl_->scene) {
        AddSceneStats(stats, impl_->scene->GetRenderStats());
    }

    const auto &platform = impl_->platform_status;
    stats.frame_callbacks_done = static_cast<std::uint64_t>(platform.frame_done_count);
    stats.surface_state_commits = platform.surface_state_commits;
    stats.surface_pixel_commits = platform.surface_pixel_commits;
    stats.surface_submission_failures = platform.surface_submission_failures;
    stats.surface_noops = platform.surface_noops;
    return stats;
}

} // namespace prism::sdk
