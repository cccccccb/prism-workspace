#include "wlr_server_internal.hpp"

namespace prism::wm {
void WlrServer::ScheduleFrames(FrameReason reason)
{
    switch (reason) {
    case FrameReason::Layout:
        ++frame_work_.layout_requests;
        break;
    case FrameReason::Effects:
        ++frame_work_.effects_requests;
        break;
    case FrameReason::Mode:
        ++frame_work_.mode_requests;
        break;
    }
    if (reason == FrameReason::Effects && wl_event_loop_) {
        // Resolve effects after surface/scene listeners settle. Only changed
        // paint nodes produce native scene damage on their affected outputs.
        if (effects_idle_) {
            return;
        }
        effects_idle_ = wl_event_loop_add_idle(wl_event_loop_, HandleEffectsIdle, this);
        if (effects_idle_) {
            return;
        }
        // Preserve progress if allocating the idle source failed.
    }
    for (const auto &output : outputs_) {
        if (output->wlr_output && output->wlr_output->enabled) {
            wlr_output_schedule_frame(output->wlr_output);
        }
    }
}

void WlrServer::InvalidateEffects()
{
    if (surface_effects_) {
        surface_effects_->MarkDirty();
    }
}

bool WlrServer::UpdateSurfaceEffects()
{
    if (!surface_effects_ || !surface_effects_->NeedsUpdate() || !scene_) {
        return false;
    }
    if (effects_idle_) {
        wl_event_source_remove(effects_idle_);
        effects_idle_ = nullptr;
    }
    const auto start = core::CurrentTimeNs();
    std::vector<WlrXdgView *> views;
    views.reserve(xdg_views_.size());
    for (auto &view : xdg_views_) {
        views.push_back(view.get());
    }
    surface_effects_->Update(scene_, views, focused_xdg_view_, theme_snapshot_.get());
    effects_cpu_.Record((core::CurrentTimeNs() - start) / 1e6);
    return true;
}

void WlrServer::HandleOutputCommit(const void *data)
{
    ++frame_work_.output_commits;
    const auto *event = static_cast<const wlr_output_event_commit *>(data);
    if (event->state->committed & WLR_OUTPUT_STATE_BUFFER) {
        ++frame_work_.output_buffer_commits;
    }
}

void WlrServer::HandleNewSurface(wlr_surface *surface)
{
    surface_watches_.emplace(surface, std::make_unique<WlrSurfaceWatch>(this, surface));
}

void WlrServer::HandleSurfaceCommit(wlr_surface *surface)
{
    ++frame_work_.surface_commits;
    // Damage describes this commit only. Preserve it before later commits and
    // before scene listeners update their surface buffers.
    if (surface_effects_) {
        surface_effects_->NotifySurfaceCommit(surface);
    }
    const auto fields = surface->current.committed;
    if (fields & WLR_SURFACE_STATE_BUFFER) {
        ++frame_work_.surface_buffer_commits;
    }
    if (fields & WLR_SURFACE_STATE_FRAME_CALLBACK_LIST) {
        ++frame_work_.surface_callback_commits;
    }
    constexpr std::uint32_t visual_fields =
        WLR_SURFACE_STATE_BUFFER | WLR_SURFACE_STATE_SURFACE_DAMAGE |
        WLR_SURFACE_STATE_BUFFER_DAMAGE | WLR_SURFACE_STATE_OPAQUE_REGION |
        WLR_SURFACE_STATE_TRANSFORM | WLR_SURFACE_STATE_SCALE | WLR_SURFACE_STATE_VIEWPORT |
        WLR_SURFACE_STATE_OFFSET;
    bool geometry_changed = false;
    if (auto it = surface_watches_.find(surface); it != surface_watches_.end()) {
        // Parent commits apply position and stacking of child surfaces without
        // a dedicated committed-field bit. Keep ordered, exact state snapshots.
        const bool children_changed = it->second->RefreshChildren();
        const bool local_changed = it->second->RefreshLocalGeometry();
        geometry_changed = children_changed || local_changed;
    }
    if (!(fields & visual_fields) && !geometry_changed) {
        ++frame_work_.surface_nonvisual_commits;
        // wlroots 0.18 scene surfaces already schedule visible callback-only
        // commits. Do not invalidate cached backgrounds for input/callback state.
        return;
    }
    // This observer runs before scene listeners. Output membership may still
    // describe the previous mapping; stage 1 conservatively invalidates all
    // visual commits, including currently hidden and newly mapped surfaces.
    InvalidateEffects();
}

void WlrServer::HandleSurfaceMapState(wlr_surface *)
{
    // Map/unmap can precede commit, and scene listeners can remove output
    // membership before this observer sees the corresponding buffer detach.
    InvalidateEffects();
}

void WlrServer::HandleSurfaceDestroy(wlr_surface *surface)
{
    if (surface_effects_) {
        surface_effects_->ForgetSurface(surface);
    }
    InvalidateEffects();
    surface_watches_.erase(surface);
}

void WlrServer::HandleNewOutput(struct wlr_output *output)
{
    wlr_output_init_render(output, allocator_, renderer_);

    if (wlr_output_is_wl(output)) {
        wlr_wl_output_set_title(output, "Project Prism Desktop");
    }

    // 1. Attach listeners
    auto wlr_out = std::make_unique<WlrOutput>(output, this);
    wlr_output_layout_add_auto(output_layout_, output);

    // 2. Attach output to hardware wlr_scene
    wlr_out->scene_output = wlr_scene_output_create(scene_, output);
    struct wlr_box box;
    wlr_output_layout_get_box(output_layout_, output, &box);
    wlr_scene_output_set_position(wlr_out->scene_output, box.x, box.y);

    // 3. Commit initial output state
    struct wlr_output_mode *mode = wlr_output_preferred_mode(output);
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    if (mode) {
        wlr_output_state_set_mode(&state, mode);
    } else if (!wl_list_empty(&output->modes)) {
        struct wlr_output_mode *first_mode = wl_container_of(output->modes.next, first_mode, link);
        wlr_output_state_set_mode(&state, first_mode);
    }
    wlr_output_commit_state(output, &state);
    wlr_output_state_finish(&state);

    if (compositor_ && output->width > 0 && output->height > 0) {
        compositor_->SetScreenSize(output->width, output->height);
    }

    PRISM_LOG_INFO("WLR-OUTPUT",
                   "New output added: %s (%dx%d @ %.1fHz) -> Native wlr_scene Attached",
                   output->name, output->width, output->height, output->refresh / 1000.0f);

    outputs_.push_back(std::move(wlr_out));
    ArrangeXdgViews();

    // The initial DRM modeset may already have a page-flip pending.
    // Let wlroots emit the frame event once the output can accept a commit.
    ScheduleFrames(FrameReason::Mode);
}

void WlrServer::RemoveOutput(WlrOutput *output)
{
    for (auto it = outputs_.begin(); it != outputs_.end(); ++it) {
        if (it->get() == output) {
            PRISM_LOG_INFO("WLR-OUTPUT", "Output removed: %s", output->wlr_output->name);
            outputs_.erase(it);
            ArrangeXdgViews();
            break;
        }
    }
}

void WlrServer::InitSceneGraph()
{
    if (!scene_) {
        return;
    }

    // 1. Layer 0: Background Desktop
    background_tree_ = wlr_scene_tree_create(&scene_->tree);

    // 2. Layer 1: Windows & Application Views
    windows_tree_ = wlr_scene_tree_create(&scene_->tree);

    // 3. Layer 2: Desktop Chrome (Top Menu Bar, Split Divider, Dock)
    chrome_tree_ = wlr_scene_tree_create(&scene_->tree);

    // 4. Layer 3: HUD (Debug Performance Overlay)
    hud_tree_ = wlr_scene_tree_create(&scene_->tree);
    wlr_scene_node_set_enabled(&hud_tree_->node, compositor_ && compositor_->IsDebugHudEnabled());
    const float hud_bg[4] = {0.05f, 0.07f, 0.09f, 0.92f};
    hud_bg_rect_ = wlr_scene_rect_create(hud_tree_, 440, 160, hud_bg);
    const float hud_border[4] = {0.35f, 0.65f, 1.0f, 1.0f}; // Neon cyan accent
    hud_border_rect_ = wlr_scene_rect_create(hud_tree_, 440, 3, hud_border);
    const float hud_fps_pill[4] = {0.25f, 0.73f, 0.31f, 1.0f}; // Bright emerald green
    hud_status_pill_ = wlr_scene_rect_create(hud_tree_, 12, 12, hud_fps_pill);
    wlr_scene_node_set_position(&hud_status_pill_->node, 16, 16);
    wlr_scene_node_set_position(&hud_tree_->node, 18, 44);
}

void WlrServer::UpdateSceneGraph(int width, int height, float dt)
{
    if (width <= 0 || height <= 0 || !hud_tree_) {
        return;
    }
    // Real XDG view geometry is owned exclusively by the BSP arrangement.
    // Decoration/material buffers are composed by SurfaceEffects below views.
    wlr_scene_node_set_enabled(&hud_tree_->node, compositor_ && compositor_->IsDebugHudEnabled());
}

void WlrServer::HandleOutputFrame(WlrOutput *output)
{
    if (!output || !output->scene_output) {
        return;
    }
    ++frame_work_.frame_events;
    const auto frame_start = core::CurrentTimeNs();
    // A backend event may precede the queued idle pass. Resolve pending effects
    // here once, then evaluate the output's actual damage again.
    const bool effects_updated = UpdateSurfaceEffects();
    // The headless backend can emit periodic frame events while idle. Damage
    // and needs_frame are the same wlroots 0.18 gates used by scene commit.
    // Callback-only commits set needs_frame through wlr_scene_surface.
    const bool output_work =
        output->wlr_output->needs_frame ||
        pixman_region32_not_empty(&output->scene_output->pending_commit_damage);
    if (!output_work) {
        ++frame_work_.idle_skips;
        if (effects_updated) {
            frame_cpu_.Record((core::CurrentTimeNs() - frame_start) / 1e6);
        }
        return;
    }
    static int s_frame_log_count = 0;
    uint64_t now_ns = core::CurrentTimeNs();
    float dt = 0.0f;
    if (output->last_frame_ns > 0) {
        dt = static_cast<float>(now_ns - output->last_frame_ns) / 1e9f;
        if (dt > 0.0001f && dt < 1.0f) {
            float inst_fps = 1.0f / dt;
            current_fps_ =
                (current_fps_ <= 0.0f) ? inst_fps : (0.9f * current_fps_ + 0.1f * inst_fps);
        }
    }
    output->last_frame_ns = now_ns;
    last_frame_time_ns_ = now_ns;
    frame_count_++;

    if (compositor_) {
        compositor_->SetPerformanceStats(current_fps_, dt, frame_count_);
        // A long idle gap is not a physics step. Native views are event-owned;
        // future animation demand must use its own monotonic time origin.
        compositor_->Tick(std::min(dt, 0.05f));
    }

    int out_w = output->wlr_output->width;
    int out_h = output->wlr_output->height;
    bool commit_ok = false;
    if (out_w > 0 && out_h > 0) {
        // 1. Update Hardware GPU Scene-graph layout (Windows, Divider, Dock, HUD)
        UpdateSceneGraph(out_w, out_h, dt);

        // 2. Hardware-accelerated GPU render & commit (wlr_scene natively dispatches GLES2 render
        // pass & Direct Scanout!)
        uint64_t t_gpu_start = core::CurrentTimeNs();
        const auto commit_sequence = output->wlr_output->commit_seq;
        ++frame_work_.scene_commit_calls;
        commit_ok = wlr_scene_output_commit(output->scene_output, nullptr);
        uint64_t t_gpu_done = core::CurrentTimeNs();
        commit_cpu_.Record((t_gpu_done - t_gpu_start) / 1e6);
        if (commit_ok) {
            ++commit_successes_;
        } else {
            ++commit_failures_;
        }
        if (commit_ok && commit_sequence == output->wlr_output->commit_seq) {
            ++frame_work_.scene_commit_noops;
        }

        // 3. Send frame_done to client surfaces
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (commit_ok) {
            wlr_scene_output_send_frame_done(output->scene_output, &now);
            ++frame_work_.frame_done_dispatches;
        }

        if (++s_frame_log_count % 60 == 1) {
            float gpu_ms = static_cast<float>(t_gpu_done - t_gpu_start) / 1e6f;
            PRISM_LOG_INFO("WLR-SCENE",
                           "Native GPU Frame %lu on '%s': commit=%s (%dx%d @ %.1f FPS, "
                           "commit_cpu=%.2fms, dt=%.2fms)",
                           frame_count_, output->wlr_output->name, commit_ok ? "OK" : "FAILED",
                           out_w, out_h, current_fps_, gpu_ms, dt * 1000.0f);
        }
    }

    frame_cpu_.Record((core::CurrentTimeNs() - frame_start) / 1e6);

    // The scene/backend schedule real damage and callback demand. Successful
    // presentation alone does not create a new frame request.
}

std::vector<OutputInfo> WlrServer::GetOutputsInfo() const
{
    std::vector<OutputInfo> result;
    for (const auto &out : outputs_) {
        struct wlr_output *w_out = out->wlr_output;
        OutputInfo info;
        info.name = w_out->name ? w_out->name : "unknown";
        info.make = w_out->make ? w_out->make : "generic";
        info.model = w_out->model ? w_out->model : "display";
        info.width = w_out->width;
        info.height = w_out->height;
        info.refresh_mhz = w_out->refresh;
        info.refresh_hz = w_out->refresh / 1000.0f;
        info.current_fps = current_fps_;
        info.adaptive_sync = (w_out->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED);

        struct wlr_output_mode *m;
        wl_list_for_each(m, &w_out->modes, link)
        {
            OutputModeInfo mi;
            mi.width = m->width;
            mi.height = m->height;
            mi.refresh_mhz = m->refresh;
            mi.refresh_hz = m->refresh / 1000.0f;
            mi.preferred = m->preferred;
            mi.current = (m == w_out->current_mode);
            info.modes.push_back(mi);
        }

        if (info.modes.empty() && info.width > 0 && info.height > 0) {
            OutputModeInfo mi;
            mi.width = info.width;
            mi.height = info.height;
            mi.refresh_mhz = info.refresh_mhz > 0 ? info.refresh_mhz : 60000;
            mi.refresh_hz = mi.refresh_mhz / 1000.0f;
            mi.preferred = true;
            mi.current = true;
            info.modes.push_back(mi);
        }

        result.push_back(info);
    }
    return result;
}

bool WlrServer::SetOutputMode(const std::string &name, int width, int height, int refresh_mhz)
{
    bool any_success = false;
    for (auto &out : outputs_) {
        struct wlr_output *w_out = out->wlr_output;
        if (!name.empty() && name != "all" && name != w_out->name) {
            continue;
        }

        struct wlr_output_mode *best_mode = nullptr;
        struct wlr_output_mode *m;
        wl_list_for_each(m, &w_out->modes, link)
        {
            if (m->width == width && m->height == height) {
                if (refresh_mhz <= 0 || std::abs(m->refresh - refresh_mhz) < 500) {
                    best_mode = m;
                    break;
                }
            }
        }

        struct wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, true);

        if (best_mode) {
            wlr_output_state_set_mode(&state, best_mode);
            PRISM_LOG_INFO("WLR-MODE", "Output '%s' mode set to fixed mode %dx%d @ %.1fHz",
                           w_out->name, best_mode->width, best_mode->height,
                           best_mode->refresh / 1000.0f);
        } else {
            bool has_modes = !wl_list_empty(&w_out->modes);
            int ref = refresh_mhz > 0
                          ? refresh_mhz
                          : (has_modes ? (w_out->refresh > 0 ? w_out->refresh : 60000) : 0);
            wlr_output_state_set_custom_mode(&state, width, height, ref);
            PRISM_LOG_INFO("WLR-MODE", "Output '%s' mode set to custom mode %dx%d (refresh=%d)",
                           w_out->name, width, height, ref);
        }

        if (w_out->render_format) {
            wlr_output_state_set_render_format(&state, w_out->render_format);
        }

        if (wlr_output_commit_state(w_out, &state)) {
            any_success = true;
            ArrangeXdgViews();
            UpdateSceneGraph(w_out->width, w_out->height);
            InvalidateEffects();
            ScheduleFrames(FrameReason::Mode);
        } else {
            PRISM_LOG_ERROR("WLR-MODE", "Failed to commit mode change on output '%s'", w_out->name);
        }
        wlr_output_state_finish(&state);
    }
    return any_success;
}

bool WlrServer::SetAdaptiveSync(const std::string &name, bool enabled)
{
    bool any_success = false;
    for (auto &out : outputs_) {
        struct wlr_output *w_out = out->wlr_output;
        if (!name.empty() && name != "all" && name != w_out->name) {
            continue;
        }

        struct wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_adaptive_sync_enabled(&state, enabled);
        if (wlr_output_commit_state(w_out, &state)) {
            any_success = true;
            PRISM_LOG_INFO("WLR-MODE", "Adaptive sync on '%s' set to %s", w_out->name,
                           enabled ? "enabled" : "disabled");
        }
        wlr_output_state_finish(&state);
    }
    return any_success;
}

void WlrServer::HandleEffectsWake()
{
    ScheduleFrames(FrameReason::Effects);
}

void WlrServer::HandleEffectsIdle(void *data)
{
    auto *server = static_cast<WlrServer *>(data);
    server->effects_idle_ = nullptr;
    server->UpdateSurfaceEffects();
}

} // namespace prism::wm
