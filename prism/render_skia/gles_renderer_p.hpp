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
    struct Image {
        sk_sp<SkImage> texture;
        std::uint64_t generation{};
    };

    const SkImage *Find(contracts::ResourceId id) const noexcept override;
    bool Current() const noexcept;
    void PruneImages();

    detail::ResourceTable resources;
    sk_sp<GrDirectContext> context;
    sk_sp<SkSurface> surface;
    std::unordered_map<std::uint64_t, Image> images;
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext egl_context = EGL_NO_CONTEXT;
    int width{}, height{};
    GLint framebuffer{-1};
    GlesRenderStats stats{};
};
} // namespace prism::render_skia
