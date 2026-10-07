#include "client_render_owner_p.hpp"

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace prism::sdk {

platform::SubmitResult ClientRenderOwner::PrepareSubmit(const platform::SubmitRequest &request)
{
    root_target_.prepared_damage.reset();
    root_target_.prepared_frame.reset();
    root_target_.prepared_target = {};
    if (terminal_.Reason() != runtime::TerminalReason::None || failed_) {
        return platform::SubmitResult::Failed;
    }
    if (terminal_.StopRequested()) {
        // Close is an independent control transition, not a failed pixel
        // submission. Deferred prevents even a pending State commit while Run
        // leaves the current Wayland pump.
        return platform::SubmitResult::Deferred;
    }

    // A popped input event remains outstanding until the UI has updated its
    // Scene and published any replacement frame. Queue emptiness is not proof.
    if (processed_event_sequence_ != issued_event_sequence_ || commands_.Size() != 0) {
        return platform::SubmitResult::AwaitFrame;
    }

    if (animation_sampling_active_ && frame_opportunity_ &&
        frame_opportunity_->configure_count != window_.ConfigureCount()) {
        ResetFrameOpportunity();
    }

    const auto frame = root_target_.render_frame;
    const auto metrics = window_.Metrics();
    if (!frame || frame->ui != installed_ui_ ||
        frame->configure_count != window_.ConfigureCount() ||
        frame->buffer_size.width != static_cast<std::uint32_t>(request.width) ||
        frame->buffer_size.height != static_cast<std::uint32_t>(request.height) ||
        frame->scale != metrics.scale) {
        if (animation_sampling_active_ && request.allow_pixels && !popup_.FrameCallbackPending() &&
            !frame_opportunity_ && !spontaneous_animation_frame_allowed_ &&
            !IssueFrameOpportunity()) {
            return platform::SubmitResult::Failed;
        }
        return platform::SubmitResult::AwaitFrame;
    }
    const bool pixels =
        request.force_pixels || !root_target_.committed_frame ||
        frame->ui != root_target_.committed_frame->ui ||
        frame->display_list != root_target_.committed_frame->display_list ||
        frame->resource_epoch != root_target_.committed_frame->resource_epoch ||
        frame->buffer_size.width != root_target_.committed_frame->buffer_size.width ||
        frame->buffer_size.height != root_target_.committed_frame->buffer_size.height ||
        frame->scale != root_target_.committed_frame->scale;

    // Both targets share one sampling permit. List equality after a root
    // commit does not prove that the popup owns a future callback: its last
    // contribution may have been State or checked-identical None.
    if (animation_sampling_active_ && request.allow_pixels && !popup_.FrameCallbackPending() &&
        !frame_opportunity_ && !spontaneous_animation_frame_allowed_ && !approved_frame_ &&
        !IssueFrameOpportunity()) {
        return platform::SubmitResult::Failed;
    }

    // A compatible State-only packet may update surface metadata even while
    // an animated pixel frame waits for a callback or for the UI's response.
    if (animation_sampling_active_ && pixels && request.allow_pixels &&
        (!approved_frame_ || frame != approved_frame_)) {
        // A frame callback is only a permit. It cannot replay the previous
        // sample while the UI computes this opportunity's current value.
        return platform::SubmitResult::AwaitFrame;
    }

    if (pixels && !request.allow_pixels) {
        return platform::SubmitResult::None;
    }
    if (pixels && (!frame->display_list || !frame->image_uses)) {
        return platform::SubmitResult::AwaitFrame;
    }

    try {
        if (!frame->input_snapshot || !frame->input_snapshot->scene ||
            !frame->input_snapshot->version) {
            throw std::runtime_error("Frame has no immutable input geometry");
        }
        const contracts::BufferSize size{static_cast<std::uint32_t>(request.width),
                                         static_cast<std::uint32_t>(request.height)};
        if (pixels) {
            if (!EnsureRenderer(request.width, request.height)) {
                throw std::runtime_error("EGL renderer unavailable");
            }
            if (!ImagesUploaded(*frame)) {
                for (const auto &use : *frame->image_uses) {
                    const auto found = render_images_.find(use.id.value);
                    if (found == render_images_.end() || found->second.version != use) {
                        throw std::runtime_error("Frame image version unavailable");
                    }
                    QueueImageUpload(use.id);
                }
                return platform::SubmitResult::Deferred;
            }

            const auto old_size = root_target_.damage_history.Size();
            const bool resized = old_size.width != size.width || old_size.height != size.height;
            if (resized) {
                root_target_.damage_history.Reset(size);
            }
            auto content = resized || frame->scale != 1.0
                               ? contracts::DamageRegion::Full()
                               : damage_commands_->CompareDamage(
                                     root_target_.committed_frame
                                         ? root_target_.committed_frame->display_list.get()
                                         : nullptr,
                                     *frame->display_list, request.width, request.height,
                                     root_target_.committed_damage_resource_epoch);

            ++root_target_.backend_stats.buffer_age_queries;
            const auto age = root_target_.egl.QueryBufferAge();
            root_target_.backend_stats.last_buffer_age = age.value_or(-1);
            if (!age || *age == 0) {
                ++root_target_.backend_stats.unknown_buffer_ages;
            }

            root_target_.prepared_damage = root_target_.damage_history.Plan(
                content, age && *age >= 0 ? std::optional<unsigned>(static_cast<unsigned>(*age))
                                          : std::nullopt);
            if (!config_.partial_rendering || frame->scale != 1.0) {
                root_target_.prepared_damage->repair_damage = contracts::DamageRegion::Full();
            }
            root_target_.prepared_content_area =
                runtime::DamageArea(root_target_.prepared_damage->content_damage, size);
            root_target_.prepared_target = root_target_.egl.TargetIdentity();

            if (root_target_.egl.SetDamage(root_target_.prepared_damage->repair_damage) ==
                platform::DamageRegionResult::Failed) {
                throw std::runtime_error("EGL repair declaration failed");
            }
        }

        window_.SetSurfaceEffects(frame->surface_effects);
        window_.SetInputRegions(frame->input_regions);
        root_target_.prepared_frame = frame;
        if (!pixels) {
            return window_.SurfaceStatePending() ? platform::SubmitResult::State
                                                 : platform::SubmitResult::None;
        }

        ++root_target_.backend_stats.gpu_render_attempts;
        const auto &repair = root_target_.prepared_damage->repair_damage;
        const auto area = runtime::DamageArea(repair, size);
        const auto started = std::chrono::steady_clock::now();
        const bool rendered = renderer_->Render(*frame->display_list, root_target_.prepared_target,
                                                request.width, request.height, repair);
        if (!render_sampled_) {
            startup_stats_.first_render_us =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::steady_clock::now() - started)
                                               .count());
            render_sampled_ = true;
        }
        if (!rendered) {
            throw std::runtime_error("GPU rendering failed");
        }

        ++root_target_.backend_stats.gpu_render_successes;
        if (repair.full) {
            ++root_target_.backend_stats.full_pixel_repairs;
        } else if (repair.rects.empty()) {
            ++root_target_.backend_stats.empty_pixel_repairs;
        } else {
            ++root_target_.backend_stats.partial_pixel_repairs;
        }
        root_target_.backend_stats.pixel_repair_pixels += area;
        return platform::SubmitResult::Pixels;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[prism-sdk] render submission failed: %s\n", error.what());
        terminal_.Fail(runtime::TerminalReason::RenderFailure);
        failed_ = true;
        CloseGpu();
        return platform::SubmitResult::Failed;
    } catch (...) {
        terminal_.Fail(runtime::TerminalReason::RenderFailure);
        failed_ = true;
        CloseGpu();
        return platform::SubmitResult::Failed;
    }
}

bool ClientRenderOwner::CommitPixels()
{
    ++root_target_.backend_stats.swap_attempts;

    try {
        bool swapped = false;
        if (root_target_.prepared_damage && root_target_.prepared_frame &&
            root_target_.prepared_target &&
            root_target_.prepared_target == root_target_.egl.TargetIdentity()) {
            const auto started = std::chrono::steady_clock::now();
            swapped = root_target_.egl.Swap(root_target_.prepared_damage->content_damage);
            if (!swap_sampled_) {
                startup_stats_.first_swap_us = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count());
                swap_sampled_ = true;
            }
        }
        if (!swapped) {
            throw std::runtime_error("EGL swap failed");
        }

        ++root_target_.backend_stats.swap_successes;
        ++root_target_.presented;
        if (!root_target_.damage_history.Commit(std::move(*root_target_.prepared_damage))) {
            throw std::runtime_error("Damage history commit failed");
        }
        root_target_.prepared_damage.reset();
        root_target_.prepared_target = {};
        ++root_target_.backend_stats.damage_history_commits;
        root_target_.backend_stats.content_damage_pixels += root_target_.prepared_content_area;
        return true;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[prism-sdk] render swap failed: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "[prism-sdk] render swap failed\n");
    }

    terminal_.Fail(runtime::TerminalReason::RenderFailure);
    failed_ = true;
    CloseGpu();
    return false;
}

void ClientRenderOwner::Submitted(platform::SubmitResult result) noexcept
{
    if (result == platform::SubmitResult::Failed) {
        terminal_.Fail(runtime::TerminalReason::RenderFailure);
        failed_ = true;
        CloseGpu();
        return;
    }

    runtime::SubmittedFrameEvent submitted;
    submitted.frame = root_target_.prepared_frame;
    submitted.metadata_prepared = static_cast<bool>(submitted.frame);
    if (submitted.frame) {
        submitted.ui = submitted.frame->ui;
        submitted.frame_sequence = submitted.frame->sequence;
        submitted.scene_revision = submitted.frame->scene_revision;
        submitted.pixels_revision = submitted.frame->pixels_revision;
        submitted.theme_generation = submitted.frame->theme_generation;
    }
    if (result == platform::SubmitResult::Pixels) {
        root_target_.committed_frame = std::move(root_target_.prepared_frame);
        root_target_.committed_damage_resource_epoch = damage_commands_->ResourceEpoch();
        submitted.kind = runtime::SubmittedKind::Pixels;
        submitted.submission = window_.LastPixelSubmission();
        submitted.feedback_expected = window_.PresentationPending(submitted.submission);

    } else if (result == platform::SubmitResult::State) {
        submitted.kind = runtime::SubmittedKind::State;
    }

    if (submitted.metadata_prepared) {
        // Only successful Pixels/State or checked-identical None reaches here.
        // Keep this reference even without queued input; future events use it.
        AdoptPopupCleanParent(*submitted.frame);
        root_target_.input_snapshot = submitted.frame->input_snapshot;
        root_target_.input_ui = submitted.frame->ui;
        ConsumeApprovedRoot(submitted.frame);
    }
    root_target_.prepared_frame.reset();

    try {
        // TrySubmit can commit immediately before WaylandWindow::Pump enters
        // its wait. Publish counters before the UI milestone callback reads
        // them, while keeping Submitted ahead of its presentation feedback.
        PublishStatus();
        QueueEvent(runtime::RenderEvent(std::move(submitted)));
        if (animation_sampling_active_ && result == platform::SubmitResult::Pixels) {
            // Keep one request parked behind the current frame callback.
            // Once the callback and feedback capacity release, PrepareSubmit
            // sends a fresh opportunity instead of resubmitting old pixels.
            window_.RequestUpdate(true);
        }
    } catch (...) {
        terminal_.Fail(runtime::TerminalReason::EventQueueFailure);
    }
}

} // namespace prism::sdk
