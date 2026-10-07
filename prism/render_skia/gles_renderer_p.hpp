#pragma once

#include "image_provider_p.hpp"
#include "include/core/SkImage.h"
#include "include/core/SkSurface.h"
#include "include/gpu/GrDirectContext.h"
#include "prism/render_skia/gles_renderer.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include "resource_table_p.hpp"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <unordered_map>

namespace prism::render_skia {
struct GlesRenderer::Impl final : detail::ImageProvider {
    struct Target {
        contracts::GpuTargetIdentity identity;
        sk_sp<SkSurface> surface;
        EGLSurface draw_surface{EGL_NO_SURFACE};
        EGLSurface read_surface{EGL_NO_SURFACE};
        int width{}, height{}, samples{}, stencil{};
        GLint framebuffer{-1};
    };

    struct Image {
        sk_sp<SkImage> texture;
        std::uint64_t generation{};
    };

    const SkImage *Find(contracts::ResourceId id) const noexcept override;
    bool Current() const noexcept;
    void PruneImages();
    Target *SelectTarget(const contracts::GpuTargetIdentity *identity, int width, int height);
    bool WrapTarget(Target &target);

    detail::ResourceTable resources;
    sk_sp<GrDirectContext> context;
    Target legacy_target;
    std::unordered_map<std::uint64_t, Target> targets;
    std::unordered_map<std::uint64_t, Image> images;
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext egl_context = EGL_NO_CONTEXT;
    EGLSurface creation_draw_surface{EGL_NO_SURFACE};
    EGLSurface creation_read_surface{EGL_NO_SURFACE};
    std::uint64_t context_lifetime_id{};
    contracts::GpuTargetIdentity active_target{};
    bool active_target_valid{false};
    GlesRenderStats stats{};
};
} // namespace prism::render_skia
