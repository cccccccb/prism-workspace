#include "prism/render_skia/raster_renderer.hpp"
#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkImage.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <png.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace prism::render_skia {

struct RasterRenderer::Impl {
    FT_Library freetype{nullptr};
    FT_Face face{nullptr};
    sk_sp<SkTypeface> typeface;
    std::unordered_map<std::uint64_t, sk_sp<SkImage>> images;
    ~Impl() {
        if (face) FT_Done_Face(face);
        if (freetype) FT_Done_FreeType(freetype);
    }
};

namespace {
SkColor ToSkColor(contracts::Color color) {
    return SkColorSetARGB(color.a, color.r, color.g, color.b);
}
SkRect ToSkRect(contracts::LogicalRect rect) {
    return SkRect::MakeXYWH(static_cast<SkScalar>(rect.x), static_cast<SkScalar>(rect.y),
                            static_cast<SkScalar>(rect.width), static_cast<SkScalar>(rect.height));
}
bool ValidRect(contracts::LogicalRect r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) &&
           std::isfinite(r.height) && r.width >= 0 && r.height >= 0 &&
           std::abs(r.x) <= 1e6 && std::abs(r.y) <= 1e6 && r.width <= 1e6 && r.height <= 1e6;
}
bool Validate(const contracts::DisplayList& list,
              const std::unordered_map<std::uint64_t, sk_sp<SkImage>>& images) {
    if (!list.window || list.commands.size() > 100000) return false;
    std::vector<bool> stack;
    for (const auto& command : list.commands) {
        if (auto* rect = std::get_if<contracts::FillRect>(&command)) {
            if (!ValidRect(rect->bounds)) return false;
        } else if (auto* rect = std::get_if<contracts::FillRoundedRect>(&command)) {
            if (!ValidRect(rect->bounds) || !std::isfinite(rect->radius) || rect->radius < 0) return false;
        } else if (auto* image = std::get_if<contracts::DrawImage>(&command)) {
            if (!image->image || !ValidRect(image->destination)) return false;
            if (!images.contains(image->image.value)) return false;
        } else if (auto* run = std::get_if<contracts::DrawGlyphRun>(&command)) {
            if (run->font.value != 1 || run->glyphs.size() > 100000 ||
                !std::isfinite(run->font_size) || run->font_size <= 0 || run->font_size > 512) return false;
            for (auto glyph : run->glyphs)
                if (glyph.glyph_index > UINT16_MAX || !std::isfinite(glyph.origin.x) ||
                    !std::isfinite(glyph.origin.y)) return false;
        } else if (auto* clip = std::get_if<contracts::PushClipRect>(&command)) {
            if (!ValidRect(clip->bounds) || stack.size() >= 256) return false;
            stack.push_back(true);
        } else if (auto* transform = std::get_if<contracts::PushTransform>(&command)) {
            if (stack.size() >= 256) return false;
            for (double v : transform->values) if (!std::isfinite(v) || std::abs(v) > 1e6) return false;
            stack.push_back(false);
        } else if (std::holds_alternative<contracts::PopClip>(command)) {
            if (stack.empty() || !stack.back()) return false;
            stack.pop_back();
        } else if (std::holds_alternative<contracts::PopTransform>(command)) {
            if (stack.empty() || stack.back()) return false;
            stack.pop_back();
        }
    }
    return stack.empty();
}
} // namespace

RasterRenderer::RasterRenderer(std::string font_path) : impl_(std::make_unique<Impl>()) {
    if (FT_Init_FreeType(&impl_->freetype) != 0) return;
    if (FT_New_Face(impl_->freetype, font_path.c_str(), 0, &impl_->face) != 0) return;
    impl_->typeface = SkTypeface::MakeFromFile(font_path.c_str());
}
RasterRenderer::~RasterRenderer() = default;
bool RasterRenderer::Ready() const { return impl_->face && impl_->typeface; }

std::optional<runtime::DecodedImage> RasterRenderer::DecodePng(const std::string& path) {
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&png, path.c_str())) return std::nullopt;
    if (png.width == 0 || png.height == 0 || png.width > 4096 || png.height > 4096 ||
        static_cast<std::size_t>(png.width) * png.height * 4 > 64 * 1024 * 1024) {
        png_image_free(&png);
        return std::nullopt;
    }
    png.format = PNG_FORMAT_RGBA;
    runtime::DecodedImage decoded;
    decoded.width = png.width;
    decoded.height = png.height;
    decoded.rgba.resize(static_cast<std::size_t>(png.width) * png.height * 4);
    if (!png_image_finish_read(&png, nullptr, decoded.rgba.data(), 0, nullptr)) {
        png_image_free(&png);
        return std::nullopt;
    }
    png_image_free(&png);
    return decoded;
}

bool RasterRenderer::RegisterImage(contracts::ResourceId id, const runtime::DecodedImage& image) {
    if (!id || image.width == 0 || image.height == 0 || image.width > 4096 || image.height > 4096 ||
        image.rgba.size() != static_cast<std::size_t>(image.width) * image.height * 4) return false;
    auto info = SkImageInfo::Make(static_cast<int>(image.width), static_cast<int>(image.height),
                                  kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    SkPixmap pixmap(info, image.rgba.data(), static_cast<std::size_t>(image.width) * 4);
    auto sk_image = SkImages::RasterFromPixmapCopy(pixmap);
    if (!sk_image) return false;
    impl_->images[id.value] = std::move(sk_image);
    return true;
}

runtime::ShapedText RasterRenderer::Shape(std::string_view text, double size) const {
    runtime::ShapedText result;
    if (!Ready() || !std::isfinite(size) || size <= 0 || size > 512) return result;
    if (FT_Set_Char_Size(impl_->face, 0, static_cast<FT_F26Dot6>(size * 64), 0, 0) != 0) return result;
    hb_font_t* font = hb_ft_font_create_referenced(impl_->face);
    if (!font) return result;
    hb_buffer_t* buffer = hb_buffer_create();
    hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()), 0, static_cast<int>(text.size()));
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(font, buffer, nullptr, 0);
    unsigned count = 0;
    auto* infos = hb_buffer_get_glyph_infos(buffer, &count);
    auto* positions = hb_buffer_get_glyph_positions(buffer, &count);
    double cursor_x = 0, cursor_y = size;
    result.glyphs.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        result.glyphs.push_back({infos[i].codepoint,
            {cursor_x + positions[i].x_offset / 64.0, cursor_y - positions[i].y_offset / 64.0}});
        cursor_x += positions[i].x_advance / 64.0;
        cursor_y -= positions[i].y_advance / 64.0;
    }
    result.width = std::max(0.0, cursor_x);
    result.height = size * 1.4;
    hb_buffer_destroy(buffer);
    hb_font_destroy(font);
    return result;
}

bool RasterRenderer::Render(const contracts::DisplayList& list, void* pixels,
                            int width, int height, int stride) const {
    if (!pixels || width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
        stride < width * 4) return false;
    const auto info = SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType);
    auto surface = SkSurfaces::WrapPixels(info, pixels, static_cast<std::size_t>(stride));
    if (!surface) return false;
    return Replay(list, surface->getCanvas());
}

bool RasterRenderer::Replay(const contracts::DisplayList& list, SkCanvas* canvas) const {
    if (!Ready() || !canvas || !Validate(list, impl_->images)) return false;
    canvas->clear(SK_ColorBLACK);
    for (const auto& command : list.commands) {
        if (auto* rect = std::get_if<contracts::FillRect>(&command)) {
            SkPaint paint;
            paint.setAntiAlias(true);
            paint.setColor(ToSkColor(rect->color));
            canvas->drawRect(ToSkRect(rect->bounds), paint);
        } else if (auto* rect = std::get_if<contracts::FillRoundedRect>(&command)) {
            SkPaint paint;
            paint.setAntiAlias(true);
            paint.setColor(ToSkColor(rect->color));
            SkRRect rounded;
            rounded.setRectXY(ToSkRect(rect->bounds), static_cast<SkScalar>(rect->radius),
                              static_cast<SkScalar>(rect->radius));
            canvas->drawRRect(rounded, paint);
        } else if (auto* run = std::get_if<contracts::DrawGlyphRun>(&command)) {
            SkFont font(impl_->typeface, static_cast<SkScalar>(run->font_size));
            SkPaint paint;
            paint.setAntiAlias(true);
            paint.setColor(ToSkColor(run->color));
            std::vector<SkGlyphID> glyphs;
            std::vector<SkPoint> points;
            glyphs.reserve(run->glyphs.size());
            points.reserve(run->glyphs.size());
            for (const auto& glyph : run->glyphs) {
                glyphs.push_back(static_cast<SkGlyphID>(glyph.glyph_index));
                points.push_back(SkPoint::Make(static_cast<SkScalar>(glyph.origin.x),
                                               static_cast<SkScalar>(glyph.origin.y)));
            }
            if (!glyphs.empty()) canvas->drawGlyphs(static_cast<int>(glyphs.size()), glyphs.data(),
                                                      points.data(), SkPoint::Make(0, 0), font, paint);
        } else if (auto* image = std::get_if<contracts::DrawImage>(&command)) {
            canvas->drawImageRect(impl_->images.at(image->image.value), ToSkRect(image->destination),
                                  SkSamplingOptions(SkFilterMode::kLinear), nullptr);
        } else if (auto* clip = std::get_if<contracts::PushClipRect>(&command)) {
            canvas->save();
            canvas->clipRect(ToSkRect(clip->bounds));
        } else if (auto* transform = std::get_if<contracts::PushTransform>(&command)) {
            canvas->save();
            const auto& v = transform->values;
            SkMatrix matrix;
            matrix.setAll(v[0], v[1], v[2], v[3], v[4], v[5], 0, 0, 1);
            canvas->concat(matrix);
        } else if (std::holds_alternative<contracts::PopClip>(command) ||
                   std::holds_alternative<contracts::PopTransform>(command)) {
            canvas->restore();
        }
    }
    return true;
}

} // namespace prism::render_skia
