#include "gles_renderer_p.hpp"
#include "include/gpu/GrBackendSurface.h"
#include "include/gpu/ganesh/SkImageGanesh.h"

namespace prism::render_skia {
namespace {
class UploadFramebuffer {
public:
    explicit UploadFramebuffer(GrDirectContext &context) : context_(context)
    {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_);
    }

    ~UploadFramebuffer()
    {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read_));
        context_.resetContext();
    }

private:
    GrDirectContext &context_;
    GLint draw_{}, read_{};
};
} // namespace

bool GlesRenderer::Impl::Current() const noexcept
{
    return context && !context->abandoned() && eglGetCurrentDisplay() == egl_display &&
           eglGetCurrentContext() == egl_context;
}

const SkImage *GlesRenderer::Impl::Find(contracts::ResourceId id) const noexcept
{
    const auto found = images.find(id.value);
    if (found == images.end() || !found->second.generation ||
        found->second.generation != resources.ImageGeneration(id)) {
        return nullptr;
    }
    return found->second.texture.get();
}

void GlesRenderer::Impl::PruneImages()
{
    for (auto image = images.begin(); image != images.end();) {
        if (!image->second.generation ||
            image->second.generation != resources.ImageGeneration({image->first})) {
            image = images.erase(image);
        } else {
            ++image;
        }
    }
}

bool GlesRenderer::ImageUploaded(contracts::ResourceId id) const
{
    return Ready() && impl_->Find(id) != nullptr;
}

bool GlesRenderer::UploadImage(contracts::ResourceId id)
{
    auto &self = *impl_;
    if (!self.Current() || !id) {
        return false;
    }
    if (ImageUploaded(id)) {
        return true;
    }
    const auto *source = self.resources.Image(id);
    const auto generation = self.resources.ImageGeneration(id);
    if (!source || !generation) {
        return false;
    }

    UploadFramebuffer restore(*self.context);
    self.PruneImages();
    try {
        // Allocate the cache slot before allocating/submitting the GPU texture.
        auto [entry, inserted] = self.images.try_emplace(id.value);
        (void)inserted;
        ++self.stats.image_upload_attempts;
        auto texture = SkImages::TextureFromImage(self.context.get(), source, skgpu::Mipmapped::kNo,
                                                  skgpu::Budgeted::kYes);
        GrBackendTexture backend;
        // TextureFromImage can leave a deferred proxy. This official query
        // instantiates it and flushes its pending writes before success.
        if (!texture || !texture->isTextureBacked() ||
            !SkImages::GetBackendTextureFromImage(texture.get(), &backend, true) ||
            !backend.isValid()) {
            self.images.erase(entry);
            return false;
        }
        self.context->flushAndSubmit(texture);
        if (self.context->abandoned() || glGetError() != GL_NO_ERROR) {
            self.images.erase(entry);
            return false;
        }
        entry->second.texture = std::move(texture);
        entry->second.generation = generation;
        ++self.stats.image_upload_successes;
        self.stats.image_uploaded_bytes += static_cast<std::uint64_t>(source->width()) *
                                           static_cast<std::uint64_t>(source->height()) * 4;
        return true;
    } catch (...) {
        self.images.erase(id.value);
        return false;
    }
}

void GlesRenderer::ReleaseImage(contracts::ResourceId id)
{
    const auto found = impl_->images.find(id.value);
    if (found == impl_->images.end()) {
        return;
    }
    if (impl_->Current()) {
        impl_->images.erase(found);
    } else {
        // Logical invalidation is immediate. Destroying this texture in a
        // different EGL context could delete another context's same GL name.
        found->second.generation = 0;
    }
}

bool GlesRenderer::EnsureImages(const contracts::DisplayList &list)
{
    impl_->PruneImages();
    for (const auto &command : list.commands) {
        const auto *image = std::get_if<contracts::DrawImage>(&command);
        if (image && !ImageUploaded(image->image) && !UploadImage(image->image)) {
            return false;
        }
    }
    return true;
}
} // namespace prism::render_skia
