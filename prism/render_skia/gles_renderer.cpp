#include "prism/render_skia/gles_renderer.hpp"
#include "gles_renderer_p.hpp"
#include "include/core/SkColorSpace.h"
#include "include/core/SkSurface.h"
#include "include/gpu/GrBackendSurface.h"
#include "include/gpu/GrDirectContext.h"
#include "include/gpu/GrTypes.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/gl/GrGLAssembleInterface.h"
#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/buffer_damage.hpp"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <utility>

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
    impl_->creation_draw_surface = eglGetCurrentSurface(EGL_DRAW);
    impl_->creation_read_surface = eglGetCurrentSurface(EGL_READ);
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

GlesRenderer::GlesRenderer(std::string font_path, contracts::GpuTargetIdentity target,
                           GlesRendererOptions options)
    : GlesRenderer(std::move(font_path), options)
{
    if (!target) {
        Close();
        return;
    }
    impl_->context_lifetime_id = target.context_lifetime_id;
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
    impl_->targets.clear();
    impl_->legacy_target.surface.reset();
    impl_->active_target_valid = false;
    impl_->context.reset();
}

void GlesRenderer::Abandon()
{
    if (impl_->context) {
        impl_->context->abandonContext();
    }
    impl_->images.clear();
    impl_->targets.clear();
    impl_->legacy_target.surface.reset();
    impl_->active_target_valid = false;
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
    return RenderInternal(list, nullptr, width, height, repair);
}

bool GlesRenderer::Render(const contracts::DisplayList &list, contracts::GpuTargetIdentity target,
                          int width, int height)
{
    return Render(list, target, width, height, contracts::DamageRegion::Full());
}

bool GlesRenderer::Render(const contracts::DisplayList &list, contracts::GpuTargetIdentity target,
                          int width, int height, const contracts::DamageRegion &repair)
{
    return RenderInternal(list, &target, width, height, repair);
}

bool GlesRenderer::ReleaseTarget(contracts::GpuTargetIdentity target)
{
    if (!target || !impl_->Current() || target.context_lifetime_id != impl_->context_lifetime_id) {
        return false;
    }
    const auto found = impl_->targets.find(target.surface_lifetime_id);
    if (found == impl_->targets.end() || found->second.identity != target) {
        return false;
    }

    impl_->targets.erase(found);
    if (impl_->active_target_valid && impl_->active_target == target) {
        impl_->active_target_valid = false;
    }
    impl_->context->resetContext();
    ++impl_->stats.target_releases;
    return true;
}

bool GlesRenderer::Impl::WrapTarget(Target &target)
{
    try {
        GrGLFramebufferInfo info{static_cast<GrGLuint>(target.framebuffer), GL_RGBA8};
        const auto backend = GrBackendRenderTargets::MakeGL(target.width, target.height,
                                                            target.samples, target.stencil, info);
        if (!backend.isValid()) {
            return false;
        }
        target.surface =
            SkSurfaces::WrapBackendRenderTarget(context.get(), backend, kBottomLeft_GrSurfaceOrigin,
                                                kRGBA_8888_SkColorType, nullptr, nullptr);
        if (!target.surface) {
            return false;
        }
    } catch (...) {
        return false;
    }
    ++stats.target_wraps;
    return true;
}

GlesRenderer::Impl::Target *
GlesRenderer::Impl::SelectTarget(const contracts::GpuTargetIdentity *identity, int width,
                                 int height)
{
    constexpr std::size_t target_limit = 64;
    Target next;
    next.identity = identity ? *identity : contracts::GpuTargetIdentity{};
    next.draw_surface = eglGetCurrentSurface(EGL_DRAW);
    next.read_surface = eglGetCurrentSurface(EGL_READ);
    next.width = width;
    next.height = height;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &next.framebuffer);
    glGetIntegerv(GL_SAMPLES, &next.samples);
    glGetIntegerv(GL_STENCIL_BITS, &next.stencil);

    Target *existing = &legacy_target;
    if (identity) {
        if (!*identity || !context_lifetime_id ||
            identity->context_lifetime_id != context_lifetime_id) {
            return nullptr;
        }
        const auto found = targets.find(identity->surface_lifetime_id);
        if (found == targets.end()) {
            if (targets.size() >= target_limit) {
                return nullptr;
            }
            existing = nullptr;
        } else {
            existing = &found->second;
            if (identity->resize_generation < existing->identity.resize_generation ||
                next.draw_surface != existing->draw_surface ||
                next.read_surface != existing->read_surface) {
                return nullptr;
            }
        }
    } else if (context_lifetime_id || next.draw_surface != creation_draw_surface ||
               next.read_surface != creation_read_surface) {
        return nullptr;
    }

    const bool descriptor_match =
        existing && existing->surface && next.width == existing->width &&
        next.height == existing->height && next.framebuffer == existing->framebuffer &&
        next.samples == existing->samples && next.stencil == existing->stencil;
    if (identity && existing && next.identity == existing->identity && !descriptor_match) {
        return nullptr;
    }
    const bool reuse = descriptor_match && next.identity == existing->identity;
    const bool switched = !active_target_valid || active_target != next.identity;
    if (switched || !reuse) {
        // FBO 0 names another backing store after eglMakeCurrent. Ganesh's GL
        // state must be invalidated even when the framebuffer name is unchanged.
        context->resetContext();
    }

    if (reuse) {
        ++stats.target_cache_hits;
    } else {
        try {
            if (!identity) {
                if (!WrapTarget(next)) {
                    return nullptr;
                }
                legacy_target = std::move(next);
                existing = &legacy_target;
            } else {
                // Allocate a bounded slot before any GPU wrapper allocation.
                auto [slot, inserted] = targets.try_emplace(identity->surface_lifetime_id);
                if (!WrapTarget(next)) {
                    if (inserted) {
                        targets.erase(slot);
                    }
                    return nullptr;
                }
                slot->second = std::move(next);
                existing = &slot->second;
            }
        } catch (...) {
            return nullptr;
        }
    }

    active_target = existing->identity;
    active_target_valid = true;
    if (switched) {
        ++stats.target_switches;
    }
    return existing;
}

bool GlesRenderer::RenderInternal(const contracts::DisplayList &list,
                                  const contracts::GpuTargetIdentity *target, int width, int height,
                                  const contracts::DamageRegion &repair)
{
    if (!Ready() || !impl_->Current() || width <= 0 || height <= 0 || width > 4096 ||
        height > 4096) {
        return false;
    }

    // WSI MakeCurrent restores its default framebuffer even on the same
    // target. Keep Ganesh's framebuffer binding assumption in sync with it.
    impl_->context->resetContext(kRenderTarget_GrGLBackendState);
    const contracts::BufferSize size{static_cast<std::uint32_t>(width),
                                     static_cast<std::uint32_t>(height)};
    const auto normalized = RasterRenderer::ClipRepair(repair, width, height);
    if (!normalized) {
        return false;
    }
    auto *selected = impl_->SelectTarget(target, width, height);
    if (!selected) {
        return false;
    }
    const bool empty = !normalized->full && normalized->rects.empty();
    if (!empty && !EnsureImages(list)) {
        return false;
    }
    if (!RasterRenderer::Replay(list, selected->surface->getCanvas(), width, height, *normalized,
                                impl_->resources, impl_.get())) {
        return false;
    }
    if (!normalized->full && normalized->rects.empty()) {
        ++impl_->stats.empty_renders;
        return true;
    }
    impl_->context->flushAndSubmit(selected->surface.get());
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
