#include "client_application_p.hpp"
#include <algorithm>
#include <chrono>

namespace prism::sdk {
void ClientApplication::Impl::DropImage(contracts::ResourceId id)
{
    if (renderer) {
        if (egl.MakeCurrent()) {
            renderer->ReleaseImage(id);
        } else {
            renderer->Abandon();
            renderer.reset();
        }
    }
    commands.UnregisterImage(id);
    registered_images.erase(id.value);
    queued_uploads.erase(id.value);
    std::erase(upload_queue, id);
    resources.Release(id);
}

bool ClientApplication::Impl::RegisterImage(contracts::ResourceId id)
{
    if (registered_images.contains(id.value)) {
        return true;
    }
    const auto *image = resources.Get(id);
    if (!image) {
        return false;
    }
    EnsureUiWorkBudget();
    if (!commands.RegisterImage(id, *image, resources.Retain(id))) {
        return false;
    }
    ++owner_turn_registrations;
    registered_images.insert(id.value);
    ++loaded_images;
    ++install_stats.image_registrations;
    return true;
}

void ClientApplication::Impl::QueueImageUpload(contracts::ResourceId id)
{
    if ((!renderer || !renderer->ImageUploaded(id)) && queued_uploads.insert(id.value).second) {
        upload_queue.push_back(id);
    }
}

bool ClientApplication::Impl::EnsureRenderer(int width, int height)
{
    if (!window.IsConfigured() || width <= 0 || height <= 0) {
        return false;
    }
    if (!egl.Ready()) {
        bool opened;
        {
            FirstCallTimer timer(startup_stats.egl_init_us, egl_init_sampled);
            opened = egl.Open(window.Display(), window.Surface(), width, height);
        }
        if (!opened) {
            return false;
        }
        gl_renderer = egl.GlRenderer();
        const auto capability = egl.Capabilities();
        render_stats.buffer_age_supported = capability.buffer_age;
        render_stats.swap_damage_supported = capability.swap_damage;
        render_stats.partial_update_supported = capability.partial_update;
        damage_history.Reset(
            {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)});
    }
    if (!egl.Resize(width, height) || !egl.MakeCurrent()) {
        return false;
    }
    if (!renderer) {
        render_skia::GlesRendererOptions options;
        if (config.gpu_resource_cache_bytes) {
            options.resource_cache_bytes = *config.gpu_resource_cache_bytes;
        }
        FirstCallTimer timer(startup_stats.ganesh_init_us, ganesh_init_sampled);
        renderer = std::make_unique<render_skia::GlesRenderer>(commands, options);
    }
    return renderer->Ready();
}

bool ClientApplication::Impl::ImagesUploaded() const
{
    if (!renderer) {
        return scene_images.empty();
    }
    for (auto value : scene_images) {
        if (resources.State({value}) == runtime::ImageState::Ready &&
            !renderer->ImageUploaded({value})) {
            return false;
        }
    }
    return true;
}

bool ClientApplication::Impl::AdvanceImageUploads()
{
    if (upload_queue.empty() || !window.IsConfigured()) {
        return true;
    }
    EnsureUiWorkBudget();
    const auto deadline = owner_turn_deadline;
    std::size_t uploaded = owner_turn_uploads;
    std::uint64_t bytes = owner_turn_upload_bytes;
    if (uploaded >= config.install_limits.images_per_turn ||
        std::chrono::steady_clock::now() >= deadline) {
        ++install_stats.budget_yields;
        window.RequestUpdate(true);
        return true;
    }
    if (!EnsureRenderer(static_cast<int>(window.Metrics().buffer_size.width),
                        static_cast<int>(window.Metrics().buffer_size.height))) {
        return false;
    }
    while (!upload_queue.empty() && uploaded < config.install_limits.images_per_turn &&
           std::chrono::steady_clock::now() < deadline) {
        const auto id = upload_queue.front();
        const auto *image = resources.Get(id);
        if (!image || !registered_images.contains(id.value)) {
            upload_queue.pop_front();
            queued_uploads.erase(id.value);
            continue;
        }
        if (renderer->ImageUploaded(id)) {
            upload_queue.pop_front();
            queued_uploads.erase(id.value);
            continue;
        }
        const auto size = image->rgba.size();
        if (uploaded && size > config.install_limits.upload_bytes_per_turn -
                                   std::min(bytes, config.install_limits.upload_bytes_per_turn)) {
            break;
        }
        if (!renderer->UploadImage(id)) {
            return false;
        }
        ++uploaded;
        bytes += size;
        owner_turn_uploads = uploaded;
        owner_turn_upload_bytes = bytes;
        ++install_stats.image_uploads;
        install_stats.upload_bytes += size;
        install_stats.oversized_uploads += size > config.install_limits.upload_bytes_per_turn;
        upload_queue.pop_front();
        queued_uploads.erase(id.value);
    }
    RecordUiWorkBudget();
    install_stats.budget_yields += !upload_queue.empty();
    window.RequestUpdate(true);
    return true;
}
} // namespace prism::sdk
