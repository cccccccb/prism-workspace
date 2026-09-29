#include "prism/render_skia/gles_renderer.hpp"
#include "gles_renderer_p.hpp"
#include "include/core/SkColorSpace.h"
#include "include/core/SkSurface.h"
#include "include/gpu/GrBackendSurface.h"
#include "include/gpu/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/gl/GrGLAssembleInterface.h"
#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/buffer_damage.hpp"
#include <EGL/egl.h>
#include <GLES3/gl3.h>

namespace prism::render_skia {
namespace {
GrGLFuncPtr ResolveGlFunction(void *, const char *name)
{
    return eglGetProcAddress(name);
}
} // namespace

GlesRenderer::GlesRenderer(std::string font_path, GlesRendererOptions options)
    : impl_(std::make_unique<Impl>())
{
    impl_->resources.RegisterFont(contracts::ResourceId{1}, font_path);
    impl_->egl_display = eglGetCurrentDisplay();
    impl_->egl_context = eglGetCurrentContext();
    if (impl_->egl_display == EGL_NO_DISPLAY || impl_->egl_context == EGL_NO_CONTEXT) {
        return;
    }
    auto interface = GrGLMakeAssembledGLESInterface(nullptr, ResolveGlFunction);
    if (interface) {
        impl_->context = GrDirectContexts::MakeGL(std::move(interface));
    }
    if (impl_->context) {
        impl_->context->setResourceCacheLimit(options.resource_cache_bytes);
    }
}

GlesRenderer::~GlesRenderer()
{
    Close();
}

bool GlesRenderer::Ready() const
{
    return impl_->context && !impl_->context->abandoned() &&
           impl_->resources.HasFont(contracts::ResourceId{1});
}

bool GlesRenderer::RegisterFont(contracts::ResourceId id, const std::string &path)
{
    return impl_->resources.RegisterFont(id, path);
}

bool GlesRenderer::RegisterImage(contracts::ResourceId id, const runtime::DecodedImage &image)
{
    return impl_->resources.RegisterImage(id, image);
}

bool GlesRenderer::RegisterImage(contracts::ResourceId id, runtime::ImageLease image)
{
    return impl_->resources.RegisterImage(id, std::move(image));
}

void GlesRenderer::UnregisterImage(contracts::ResourceId id)
{
    impl_->resources.UnregisterImage(id);
    ReleaseImage(id);
}

void GlesRenderer::Close()
{
    if (impl_->context && (eglGetCurrentDisplay() != impl_->egl_display ||
                           eglGetCurrentContext() != impl_->egl_context)) {
        Abandon();
        return;
    }
    impl_->images.clear();
    impl_->surface.reset();
    impl_->context.reset();
}

void GlesRenderer::Abandon()
{
    if (impl_->context) {
        impl_->context->abandonContext();
    }
    impl_->images.clear();
    impl_->surface.reset();
    impl_->context.reset();
}

bool GlesRenderer::Render(const contracts::DisplayList &list, int width, int height)
{
    return Render(list, width, height, contracts::DamageRegion::Full());
}

GlesRenderStats GlesRenderer::GetRenderStats() const
{
    return impl_->stats;
}

bool GlesRenderer::Render(const contracts::DisplayList &list, int width, int height,
                          const contracts::DamageRegion &repair)
{
    if (!Ready() || eglGetCurrentDisplay() != impl_->egl_display ||
        eglGetCurrentContext() != impl_->egl_context || width <= 0 || height <= 0 || width > 4096 ||
        height > 4096) {
        return false;
    }
    const contracts::BufferSize size{static_cast<std::uint32_t>(width),
                                     static_cast<std::uint32_t>(height)};
    const auto normalized = RasterRenderer::ClipRepair(repair, width, height);
    if (!normalized) {
        return false;
    }
    const bool empty = !normalized->full && normalized->rects.empty();
    if (!empty && !EnsureImages(list)) {
        return false;
    }
    GLint framebuffer = 0, samples = 0, stencil = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_SAMPLES, &samples);
    glGetIntegerv(GL_STENCIL_BITS, &stencil);
    if (!impl_->surface || width != impl_->width || height != impl_->height ||
        framebuffer != impl_->framebuffer) {
        impl_->surface.reset();
        GrGLFramebufferInfo info{static_cast<GrGLuint>(framebuffer), GL_RGBA8};
        auto target = GrBackendRenderTargets::MakeGL(width, height, samples, stencil, info);
        if (!target.isValid()) {
            return false;
        }
        impl_->surface = SkSurfaces::WrapBackendRenderTarget(
            impl_->context.get(), target, kBottomLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType,
            nullptr, nullptr);
        if (!impl_->surface) {
            return false;
        }
        impl_->width = width;
        impl_->height = height;
        impl_->framebuffer = framebuffer;
    }
    if (!RasterRenderer::Replay(list, impl_->surface->getCanvas(), width, height, *normalized,
                                impl_->resources, impl_.get())) {
        return false;
    }
    if (!normalized->full && normalized->rects.empty()) {
        ++impl_->stats.empty_renders;
        return true;
    }
    impl_->context->flushAndSubmit(impl_->surface.get());
    if (glGetError() != GL_NO_ERROR) {
        return false;
    }
    if (normalized->full) {
        ++impl_->stats.full_renders;
    } else {
        ++impl_->stats.partial_renders;
    }
    impl_->stats.repair_pixels += runtime::DamageArea(*normalized, size);
    return true;
}
} // namespace prism::render_skia
