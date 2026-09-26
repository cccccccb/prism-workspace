#include "prism/render_skia/gles_renderer.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include "include/core/SkSurface.h"
#include "include/core/SkColorSpace.h"
#include "include/gpu/GrDirectContext.h"
#include "include/gpu/GrBackendSurface.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/gl/GrGLAssembleInterface.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>

namespace prism::render_skia {
struct GlesRenderer::Impl {
    explicit Impl(const RasterRenderer& source) : commands(source) {}
    const RasterRenderer& commands;
    sk_sp<GrDirectContext> context;
    sk_sp<SkSurface> surface;
    int width = 0;
    int height = 0;
    GLint framebuffer = -1;
};

GlesRenderer::GlesRenderer(const RasterRenderer& commands)
    : impl_(std::make_unique<Impl>(commands)) {
    auto interface = GrGLMakeAssembledGLESInterface(nullptr,
        [](void*, const char* name) { return eglGetProcAddress(name); });
    if (interface) impl_->context = GrDirectContexts::MakeGL(std::move(interface));
}
GlesRenderer::~GlesRenderer() = default;
bool GlesRenderer::Ready() const { return impl_->context != nullptr; }
void GlesRenderer::Close() {
    impl_->surface.reset();
    impl_->context.reset();
}
bool GlesRenderer::Render(const contracts::DisplayList& list, int width, int height) {
    if (!Ready() || width <= 0 || height <= 0 || width > 4096 || height > 4096) return false;
    GLint framebuffer = 0, samples = 0, stencil = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_SAMPLES, &samples);
    glGetIntegerv(GL_STENCIL_BITS, &stencil);
    if (!impl_->surface || width != impl_->width || height != impl_->height ||
        framebuffer != impl_->framebuffer) {
        impl_->surface.reset();
        GrGLFramebufferInfo info{static_cast<GrGLuint>(framebuffer), GL_RGBA8};
        auto target = GrBackendRenderTargets::MakeGL(width, height, samples, stencil, info);
        if (!target.isValid()) return false;
        impl_->surface = SkSurfaces::WrapBackendRenderTarget(impl_->context.get(), target,
            kBottomLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType, nullptr, nullptr);
        if (!impl_->surface) return false;
        impl_->width = width;
        impl_->height = height;
        impl_->framebuffer = framebuffer;
    }
    if (!impl_->commands.Replay(list, impl_->surface->getCanvas())) return false;
    impl_->context->flushAndSubmit(impl_->surface.get());
    return glGetError() == GL_NO_ERROR;
}
} // namespace prism::render_skia
