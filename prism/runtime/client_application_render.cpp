#include "client_application_p.hpp"

namespace prism::sdk {
void ClientApplication::Impl::HandleWindowEvent(const contracts::WindowEvent &event)
{
    auto &app = *this;
    if (auto *configure = std::get_if<contracts::ConfigureEvent>(&event)) {
        app.scene->SetViewport(configure->metrics.logical_size);
    } else if (auto *motion = std::get_if<contracts::PointerMotionEvent>(&event)) {
        if (app.scene->SetPointer(motion->position)) {
            app.window.RequestUpdate(true);
        }
    } else if (auto *key = std::get_if<contracts::KeyEvent>(&event)) {
        if (key->state == contracts::ButtonState::Pressed) {
            if (key->physical_key == 0x2B && app.scene->FocusNext()) {
                app.window.RequestUpdate(true);
            } else if ((key->physical_key == 0x28 || key->physical_key == 0x2C) && app.on_action) {
                if (auto action = app.scene->FocusedAction()) {
                    app.on_action(*action);
                }
            }
        }
    } else if (auto *button = std::get_if<contracts::PointerButtonEvent>(&event)) {
        if (button->state == contracts::ButtonState::Pressed &&
            button->button == contracts::PointerButton::Primary && app.on_action) {
            if (auto action = app.scene->ActionAt(button->position)) {
                app.on_action(*action);
            }
        }
    }
}

platform::SubmitResult
ClientApplication::Impl::PrepareSubmit(const platform::SubmitRequest &request)
{
    auto &app = *this;
    app.prepared_damage.reset();
    app.prepared_frame.reset();
    if (app.failed || !app.scene) {
        return platform::SubmitResult::Failed;
    }

    const auto dirty = app.scene->PendingDirty();
    const bool pixels = request.force_pixels || !app.last_list || !app.committed_frame ||
                        app.scene->PixelsRevision() != app.committed_frame->pixels_revision;

    // Keep geometry/material metadata with the corresponding new pixels.
    // A pure Composite change is allowed through an old pixel callback.
    if (pixels && !request.allow_pixels) {
        return platform::SubmitResult::None;
    }

    try {
        const contracts::BufferSize size{static_cast<std::uint32_t>(request.width),
                                         static_cast<std::uint32_t>(request.height)};
        const auto metrics = app.window.Metrics();
        std::shared_ptr<const runtime::FramePacket> frame;
        if (pixels) {
            if (!app.EnsureRenderer(request.width, request.height)) {
                throw std::runtime_error("EGL renderer unavailable");
            }
            if (!app.ImagesUploaded()) {
                for (auto value : app.scene_images) {
                    if (app.registered_images.contains(value)) {
                        app.QueueImageUpload({value});
                    }
                }
                return platform::SubmitResult::Deferred;
            }

            frame = app.CaptureFramePacket(true, size, metrics.scale, app.window.ConfigureCount());
            const auto old_size = app.damage_history.Size();
            const bool resized = old_size.width != size.width || old_size.height != size.height;
            if (resized) {
                app.damage_history.Reset(size);
            }

            // The renderer compares against the last successful submission,
            // not against the last UI frame created or skipped.
            auto content =
                resized || frame->scale != 1.0
                    ? contracts::DamageRegion::Full()
                    : app.commands.CompareDamage(
                          app.committed_frame ? app.committed_frame->display_list.get() : nullptr,
                          *frame->display_list, request.width, request.height,
                          app.committed_frame ? app.committed_frame->resource_epoch : 0);

            ++app.render_stats.buffer_age_queries;
            const auto age = app.egl.QueryBufferAge();
            app.render_stats.last_buffer_age = age.value_or(-1);
            if (!age || *age == 0) {
                ++app.render_stats.unknown_buffer_ages;
            }

            app.prepared_damage = app.damage_history.Plan(
                content, age && *age >= 0 ? std::optional<unsigned>(static_cast<unsigned>(*age))
                                          : std::nullopt);
            if (!app.config.partial_rendering || frame->scale != 1.0) {
                app.prepared_damage->repair_damage = contracts::DamageRegion::Full();
            }
            app.prepared_content_area =
                runtime::DamageArea(app.prepared_damage->content_damage, size);

            if (app.egl.SetDamage(app.prepared_damage->repair_damage) ==
                platform::DamageRegionResult::Failed) {
                throw std::runtime_error("EGL repair declaration failed");
            }

        } else if (!runtime::Has(dirty, runtime::Dirty::Composite)) {
            return platform::SubmitResult::None;
        } else {
            frame = app.CaptureFramePacket(false, size, metrics.scale, app.window.ConfigureCount());
        }

        app.window.SetSurfaceEffects(frame->surface_effects);
        app.window.SetInputRegions(frame->input_regions);
        app.prepared_frame = std::move(frame);
        if (!pixels) {
            return app.window.SurfaceStatePending() ? platform::SubmitResult::State
                                                    : platform::SubmitResult::None;
        }

        ++app.render_stats.gpu_render_attempts;
        const auto &repair = app.prepared_damage->repair_damage;
        const auto area = runtime::DamageArea(repair, {static_cast<std::uint32_t>(request.width),
                                                       static_cast<std::uint32_t>(request.height)});
        bool rendered;
        {
            FirstCallTimer timer(app.startup_stats.first_render_us, app.render_sampled);
            rendered = app.renderer->Render(*app.prepared_frame->display_list, request.width,
                                            request.height, repair);
        }
        if (!rendered) {
            throw std::runtime_error("GPU rendering failed");
        }
        ++app.render_stats.gpu_render_successes;
        if (repair.full) {
            ++app.render_stats.full_pixel_repairs;
        } else if (repair.rects.empty()) {
            ++app.render_stats.empty_pixel_repairs;
        } else {
            ++app.render_stats.partial_pixel_repairs;
        }
        app.render_stats.pixel_repair_pixels += area;
        return platform::SubmitResult::Pixels;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[prism-sdk] submission failed: %s\n", error.what());
        app.FailFrontend();
        return platform::SubmitResult::Failed;
    }
}

bool ClientApplication::Impl::CommitPixels()
{
    auto &app = *this;
    ++app.render_stats.swap_attempts;

    bool swapped = false;
    if (app.prepared_damage) {
        FirstCallTimer timer(app.startup_stats.first_swap_us, app.swap_sampled);
        swapped = app.egl.Swap(app.prepared_damage->content_damage);
    }
    if (!swapped) {
        app.FailFrontend();
        return false;
    }
    ++app.render_stats.swap_successes;
    ++app.presented;

    if (!app.damage_history.Commit(std::move(*app.prepared_damage))) {
        app.FailFrontend();
        return false;
    }
    app.prepared_damage.reset();
    ++app.render_stats.damage_history_commits;
    app.render_stats.content_damage_pixels += app.prepared_content_area;
    return true;
}

void ClientApplication::Impl::Submitted(platform::SubmitResult result)
{
    auto &app = *this;
    if (result == platform::SubmitResult::Failed) {
        app.FailFrontend();
        return;
    }

    const bool metadata_prepared = static_cast<bool>(app.prepared_frame);
    const auto submitted_ui = metadata_prepared ? app.prepared_frame->ui : runtime::UiLoadId{};
    if (result == platform::SubmitResult::Pixels) {
        app.committed_frame = std::move(app.prepared_frame);
        const auto submission = app.window.LastPixelSubmission();
        if (!app.ui_presentation.Submit(submitted_ui, submission,
                                        app.window.PresentationPending(submission))) {
            app.FailFrontend();
            return;
        }
    }

    // None can acknowledge a checked, identical metadata request, e.g.
    // when the optional effects extension is unavailable.
    if (metadata_prepared && app.scene) {
        app.scene->AcknowledgeComposite();
    }
    app.prepared_frame.reset();
    if (result == platform::SubmitResult::Pixels && app.on_ui_submitted) {
        app.on_ui_submitted(submitted_ui);
    }
}

void ClientApplication::Impl::HandlePresentation(const platform::PixelPresentation &event)
{
    const auto load = ui_presentation.Present(event);
    if (load != installed_ui || event.outcome != platform::PresentationOutcome::Discarded) {
        return;
    }
    const auto state = ui_presentation.Get(load);
    if (state.installed && !state.presented && event.submission.value == state.last_submission) {
        window.RequestRedraw(true);
    }
}

} // namespace prism::sdk
