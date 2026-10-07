#include "surface_effects_internal.hpp"

#include "surface_effects_contour_p.hpp"
#include <new>
#include <stdexcept>

namespace prism::wm {
namespace {
struct UploadState {
    GLint active{}, texture{}, unpack{};

    UploadState()
    {
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack);
        glActiveTexture(GL_TEXTURE1);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    }

    ~UploadState()
    {
        glBindTexture(GL_TEXTURE_2D, texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpack);
        glActiveTexture(active);
    }
};

GLuint UploadCoverage(const std::vector<std::uint8_t> &pixels, int width, int height)
{
    UploadState restore;
    GLuint texture{};
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, width, height, 0, GL_ALPHA, GL_UNSIGNED_BYTE,
                 pixels.data());
    if (glGetError() != GL_NO_ERROR || !texture) {
        if (texture) {
            glDeleteTextures(1, &texture);
        }
        return 0;
    }
    return texture;
}
} // namespace

effect_detail::ContourMask::~ContourMask()
{
    if (!texture || !renderer || !wlr_renderer_is_gles2(renderer)) {
        return;
    }

    ContextScope context(renderer);
    if (context.current) {
        glDeleteTextures(1, &texture);
    }
}

bool SurfaceEffects::Impl::PrepareMask(Paint &paint, const Region &region, int width, int height,
                                       double origin_x, double origin_y)
{
    if (!region.contour) {
        paint.mask.reset();
        return true;
    }
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192 ||
        std::size_t(width) * std::size_t(height) > effect_detail::ContourMaskPixelLimit ||
        region.contour->points.size() < 3 ||
        region.contour->points.size() > contracts::ContourVertexLimit) {
        ++counters.mask_failures;
        return false;
    }

    try {
        auto relative = *region.contour;
        const auto bounds = contracts::ContourBounds(relative);
        for (auto &point : relative.points) {
            point.x -= bounds.x;
            point.y -= bounds.y;
        }
        if (paint.mask && paint.mask->texture && paint.mask->width == width &&
            paint.mask->height == height && paint.mask->origin_x == origin_x &&
            paint.mask->origin_y == origin_y && paint.mask->relative == relative) {
            ++counters.mask_cache_hits;
            return true;
        }

        ContextScope context(renderer);
        if (!context.current) {
            ++counters.mask_failures;
            return false;
        }
        GLint texture_limit{};
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &texture_limit);
        if (width <= 0 || height <= 0 || width > texture_limit || height > texture_limit ||
            glGetError() != GL_NO_ERROR) {
            ++counters.mask_failures;
            return false;
        }

        const auto pixels =
            effect_detail::RasterizeContourCoverage(relative, width, height, origin_x, origin_y);
        auto mask = std::make_unique<effect_detail::ContourMask>();
        mask->renderer = renderer;
        mask->width = width;
        mask->height = height;
        mask->origin_x = origin_x;
        mask->origin_y = origin_y;
        mask->relative = std::move(relative);
        mask->texture = UploadCoverage(pixels, width, height);
        if (!mask->texture) {
            ++counters.mask_failures;
            return false;
        }

        paint.mask = std::move(mask);
        ++counters.mask_builds;
        return true;
    } catch (const std::invalid_argument &) {
        ++counters.mask_failures;
    } catch (const std::bad_alloc &) {
        ++counters.mask_failures;
    }
    return false;
}

} // namespace prism::wm
