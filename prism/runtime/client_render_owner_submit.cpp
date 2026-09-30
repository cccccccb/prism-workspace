#include "client_render_owner_p.hpp"

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace prism::sdk {

platform::SubmitResult ClientRenderOwner::PrepareSubmit(const platform::SubmitRequest &request)
{
    prepared_damage_.reset();
    prepared_frame_.reset();
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

    const auto frame = render_frame_;
    const auto metrics = window_.Metrics();
    if (!frame || frame->ui != installed_ui_ ||
        frame->configure_count != window_.ConfigureCount() ||
        frame->buffer_size.width != static_cast<std::uint32_t>(request.width) ||
        frame->buffer_size.height != static_cast<std::uint32_t>(request.height) ||
        frame->scale != metrics.scale) {
        if (animation_sampling_active_ && request.allow_pixels && !frame_opportunity_ &&
            !spontaneous_animation_frame_allowed_ && !IssueFrameOpportunity()) {
            return platform::SubmitResult::Failed;
        }
        return platform::SubmitResult::AwaitFrame;
    }
    const bool pixels = request.force_pixels || !committed_frame_ ||
                        frame->ui != committed_frame_->ui ||
                        frame->pixels_revision != committed_frame_->pixels_revision ||
                        frame->buffer_size.width != committed_frame_->buffer_size.width ||
                        frame->buffer_size.height != committed_frame_->buffer_size.height ||
                        frame->scale != committed_frame_->scale;

    if (animation_sampling_active_ && request.allow_pixels && !frame_opportunity_ &&
        !spontaneous_animation_frame_allowed_ && !approved_frame_ && !IssueFrameOpportunity()) {
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

            const auto old_size = damage_history_.Size();
            const bool resized = old_size.width != size.width || old_size.height != size.height;
            if (resized) {
                damage_history_.Reset(size);
            }
            auto content =
                resized || frame->scale != 1.0
                    ? contracts::DamageRegion::Full()
                    : damage_commands_->CompareDamage(
                          committed_frame_ ? committed_frame_->display_list.get() : nullptr,
                          *frame->display_list, request.width, request.height,
                          committed_damage_resource_epoch_);

            ++backend_stats_.buffer_age_queries;
            const auto age = egl_.QueryBufferAge();
            backend_stats_.last_buffer_age = age.value_or(-1);
            if (!age || *age == 0) {
                ++backend_stats_.unknown_buffer_ages;
            }

            prepared_damage_ = damage_history_.Plan(
                content, age && *age >= 0 ? std::optional<unsigned>(static_cast<unsigned>(*age))
                                          : std::nullopt);
            if (!config_.partial_rendering || frame->scale != 1.0) {
                prepared_damage_->repair_damage = contracts::DamageRegion::Full();
            }
            prepared_content_area_ = runtime::DamageArea(prepared_damage_->content_damage, size);

            if (egl_.SetDamage(prepared_damage_->repair_damage) ==
                platform::DamageRegionResult::Failed) {
                throw std::runtime_error("EGL repair declaration failed");
            }
        }

        window_.SetSurfaceEffects(frame->surface_effects);
        window_.SetInputRegions(frame->input_regions);
        prepared_frame_ = frame;
        if (!pixels) {
            return window_.SurfaceStatePending() ? platform::SubmitResult::State
                                                 : platform::SubmitResult::None;
        }

        ++backend_stats_.gpu_render_attempts;
        const auto &repair = prepared_damage_->repair_damage;
        const auto area = runtime::DamageArea(repair, size);
        const auto started = std::chrono::steady_clock::now();
        const bool rendered =
            renderer_->Render(*frame->display_list, request.width, request.height, repair);
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

        ++backend_stats_.gpu_render_successes;
        if (repair.full) {
            ++backend_stats_.full_pixel_repairs;
        } else if (repair.rects.empty()) {
            ++backend_stats_.empty_pixel_repairs;
        } else {
            ++backend_stats_.partial_pixel_repairs;
        }
        backend_stats_.pixel_repair_pixels += area;
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
    ++backend_stats_.swap_attempts;

    try {
        bool swapped = false;
        if (prepared_damage_) {
            const auto started = std::chrono::steady_clock::now();
            swapped = egl_.Swap(prepared_damage_->content_damage);
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

        ++backend_stats_.swap_successes;
        ++presented_;
        if (!damage_history_.Commit(std::move(*prepared_damage_))) {
            throw std::runtime_error("Damage history commit failed");
        }
        prepared_damage_.reset();
        ++backend_stats_.damage_history_commits;
        backend_stats_.content_damage_pixels += prepared_content_area_;
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
    submitted.frame = prepared_frame_;
    submitted.metadata_prepared = static_cast<bool>(submitted.frame);
    if (submitted.frame) {
        submitted.ui = submitted.frame->ui;
        submitted.frame_sequence = submitted.frame->sequence;
        submitted.scene_revision = submitted.frame->scene_revision;
        submitted.pixels_revision = submitted.frame->pixels_revision;
        submitted.theme_generation = submitted.frame->theme_generation;
    }
    if (result == platform::SubmitResult::Pixels) {
        committed_frame_ = std::move(prepared_frame_);
        committed_damage_resource_epoch_ = damage_commands_->ResourceEpoch();
        submitted.kind = runtime::SubmittedKind::Pixels;
        submitted.submission = window_.LastPixelSubmission();
        submitted.feedback_expected = window_.PresentationPending(submitted.submission);
        if (animation_sampling_active_) {
            approved_frame_.reset();
            spontaneous_animation_frame_allowed_ = false;
        }
    } else if (result == platform::SubmitResult::State) {
        submitted.kind = runtime::SubmittedKind::State;
    }
    if (animation_sampling_active_ && submitted.metadata_prepared &&
        result != platform::SubmitResult::Pixels) {
        approved_frame_.reset();
    }
    if (submitted.metadata_prepared) {
        // Only successful Pixels/State or checked-identical None reaches here.
        // Keep this reference even without queued input; future events use it.
        input_snapshot_ = submitted.frame->input_snapshot;
        input_ui_ = submitted.frame->ui;
    }
    prepared_frame_.reset();

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
