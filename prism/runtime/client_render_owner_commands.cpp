#include "client_render_owner_p.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>
#include <variant>

namespace prism::sdk {

bool ClientRenderOwner::DrainCommands()
{
    while (auto command = commands_.TryPop()) {
        if (auto *installed = std::get_if<runtime::InstallUiCommand>(&*command)) {
            if (!installed->ui.owner || !installed->ui.generation) {
                throw std::runtime_error("Invalid render UI installation");
            }
            if (installed_ui_ != installed->ui) {
                installed_ui_ = installed->ui;
                render_frame_.reset();
                committed_frame_.reset();
                committed_damage_resource_epoch_ = 0;
                prepared_frame_.reset();
                prepared_damage_.reset();
                damage_history_.Invalidate();
            }
            continue;
        }

        if (auto *invalidated = std::get_if<runtime::InvalidateFrameCommand>(&*command)) {
            if (!invalidated->ui.owner || !invalidated->ui.generation) {
                throw std::runtime_error("Invalid render frame invalidation");
            }
            if (invalidated->ui == installed_ui_) {
                // A later UI mutation superseded the candidate already read
                // from the queue. Keep the last successful pixel baseline.
                render_frame_.reset();
                window_.RequestUpdate(true);
            }
            continue;
        }

        if (auto *frame = std::get_if<runtime::FrameCommand>(&*command)) {
            if (!frame->frame || frame->frame->ui != installed_ui_) {
                throw std::runtime_error("Render frame belongs to an uninstalled UI");
            }
            render_frame_ = std::move(frame->frame);
            window_.RequestUpdate(true);
            continue;
        }

        if (auto *request = std::get_if<runtime::RequestRenderCommand>(&*command)) {
            if (request->kind == runtime::RenderRequestKind::Redraw) {
                window_.RequestRedraw(request->deferred);
            } else {
                window_.RequestUpdate(request->deferred);
            }
            continue;
        }

        if (auto *processed = std::get_if<runtime::UiEventsProcessedCommand>(&*command)) {
            if (!processed->sequence || processed->sequence < processed_event_sequence_ ||
                processed->sequence > issued_event_sequence_) {
                throw std::runtime_error("Invalid processed window event sequence");
            }
            processed_event_sequence_ = processed->sequence;
            window_.RequestUpdate(true);
            continue;
        }

        if (auto *image = std::get_if<runtime::RegisterImageCommand>(&*command)) {
            if (!RegisterImage(std::move(*image))) {
                return false;
            }
            continue;
        }

        if (!ReleaseImage(std::get<runtime::ReleaseImageCommand>(*command))) {
            return false;
        }
    }
    return true;
}

bool ClientRenderOwner::RegisterImage(runtime::RegisterImageCommand command)
{
    if (!command.version.id || !command.version.generation || !command.pixels) {
        throw std::runtime_error("Invalid render image registration");
    }
    auto found = render_images_.find(command.version.id.value);
    if (found != render_images_.end() &&
        found->second.version.generation >= command.version.generation) {
        throw std::runtime_error("Out-of-order render image registration");
    }

    if (!damage_commands_->RegisterImage(command.version.id, command.pixels)) {
        throw std::runtime_error("Render damage image registration failed");
    }
    if (renderer_) {
        if (!egl_.MakeCurrent()) {
            throw std::runtime_error("Render image registration context unavailable");
        }
        if (found != render_images_.end()) {
            renderer_->UnregisterImage(command.version.id);
        }
        if (!renderer_->RegisterImage(command.version.id, command.pixels)) {
            throw std::runtime_error("GPU image registration failed");
        }
    }

    render_images_.insert_or_assign(command.version.id.value,
                                    RenderImage{command.version, std::move(command.pixels)});
    QueueImageUpload(command.version.id);
    return true;
}

bool ClientRenderOwner::ReleaseImage(runtime::ReleaseImageCommand command)
{
    if (!command.version.id || !command.version.generation) {
        throw std::runtime_error("Invalid render image release");
    }

    auto found = render_images_.find(command.version.id.value);
    if (found != render_images_.end() && found->second.version == command.version) {
        if (renderer_ && !egl_.MakeCurrent()) {
            renderer_->Abandon();
            renderer_.reset();
            throw std::runtime_error("Render image release context unavailable");
        }
        if (renderer_) {
            renderer_->UnregisterImage(command.version.id);
        }
        damage_commands_->UnregisterImage(command.version.id);
        render_images_.erase(found);
        queued_uploads_.erase(command.version.id.value);
        std::erase(upload_queue_, command.version.id);
    }

    return QueueEvent(runtime::RenderEvent(runtime::ImageReleasedEvent{command.version}));
}

void ClientRenderOwner::QueueImageUpload(contracts::ResourceId id)
{
    if (queued_uploads_.insert(id.value).second) {
        upload_queue_.push_back(id);
    }
}

bool ClientRenderOwner::EnsureRenderer(int width, int height)
{
    if (!window_.IsConfigured() || width <= 0 || height <= 0) {
        return false;
    }
    if (!egl_.Ready()) {
        const auto started = std::chrono::steady_clock::now();
        const bool opened = egl_.Open(window_.Display(), window_.Surface(), width, height);
        if (!egl_init_sampled_) {
            startup_stats_.egl_init_us =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::steady_clock::now() - started)
                                               .count());
            egl_init_sampled_ = true;
        }
        if (!opened) {
            return false;
        }

        gl_renderer_ = egl_.GlRenderer();
        const auto capability = egl_.Capabilities();
        backend_stats_.buffer_age_supported = capability.buffer_age;
        backend_stats_.swap_damage_supported = capability.swap_damage;
        backend_stats_.partial_update_supported = capability.partial_update;
        damage_history_.Reset(
            {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)});
    }
    if (!egl_.Resize(width, height) || !egl_.MakeCurrent()) {
        return false;
    }
    if (!renderer_) {
        render_skia::GlesRendererOptions options;
        if (config_.gpu_resource_cache_bytes) {
            options.resource_cache_bytes = *config_.gpu_resource_cache_bytes;
        }

        const auto started = std::chrono::steady_clock::now();
        auto next = std::make_unique<render_skia::GlesRenderer>(config_.font_path, options);
        if (!ganesh_init_sampled_) {
            startup_stats_.ganesh_init_us =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::steady_clock::now() - started)
                                               .count());
            ganesh_init_sampled_ = true;
        }
        if (!next->Ready()) {
            return false;
        }
        for (const auto &[value, image] : render_images_) {
            if (!image.pixels || !next->RegisterImage({value}, image.pixels)) {
                return false;
            }
            QueueImageUpload({value});
        }
        renderer_ = std::move(next);
    }
    return renderer_->Ready();
}

bool ClientRenderOwner::ImagesUploaded(const runtime::FramePacket &frame) const
{
    if (!renderer_ || !frame.image_uses) {
        return false;
    }
    for (const auto &use : *frame.image_uses) {
        const auto found = render_images_.find(use.id.value);
        if (found == render_images_.end() || found->second.version != use ||
            !renderer_->ImageUploaded(use.id)) {
            return false;
        }
    }
    return true;
}

bool ClientRenderOwner::AdvanceImageUploads()
{
    if (upload_queue_.empty() || !window_.IsConfigured()) {
        return true;
    }

    const auto metrics = window_.Metrics();
    if (!EnsureRenderer(static_cast<int>(metrics.buffer_size.width),
                        static_cast<int>(metrics.buffer_size.height))) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + config_.install_limits.cpu_per_turn;
    std::size_t uploaded = 0;
    std::uint64_t bytes = 0;
    while (!upload_queue_.empty() && uploaded < config_.install_limits.images_per_turn &&
           std::chrono::steady_clock::now() < deadline) {
        const auto id = upload_queue_.front();
        const auto found = render_images_.find(id.value);
        if (found == render_images_.end() || !found->second.pixels) {
            upload_queue_.pop_front();
            queued_uploads_.erase(id.value);
            continue;
        }
        const auto size = found->second.pixels->rgba.size();
        if (uploaded && size > config_.install_limits.upload_bytes_per_turn -
                                   std::min<std::uint64_t>(
                                       bytes, config_.install_limits.upload_bytes_per_turn)) {
            break;
        }

        if (!renderer_->ImageUploaded(id)) {
            if (!renderer_->UploadImage(id) || !renderer_->ImageUploaded(id)) {
                return false;
            }
            ++uploaded;
            bytes += size;
            ++backend_stats_.image_uploads;
            backend_stats_.upload_bytes += size;
            backend_stats_.oversized_uploads += size > config_.install_limits.upload_bytes_per_turn;
        }
        if (!QueueEvent(runtime::RenderEvent(runtime::ImageUploadedEvent{found->second.version}))) {
            return false;
        }
        upload_queue_.pop_front();
        queued_uploads_.erase(id.value);
    }
    if (!upload_queue_.empty()) {
        window_.RequestUpdate(true);
    }
    return true;
}

} // namespace prism::sdk
