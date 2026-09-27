#include "prism/render_skia/raster_renderer.hpp"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRegion.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "prism/runtime/buffer_damage.hpp"
#include "vector_icons.hpp"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <hb-ft.h>
#include <limits>
#include <png.h>
#include <unordered_map>
#include <variant>
#include <vector>

namespace prism::render_skia {

struct RasterRenderer::Impl {
    struct FontResource {
        FT_Face face{nullptr};
        sk_sp<SkTypeface> typeface;
    };

    FT_Library freetype{nullptr};
    std::unordered_map<std::uint64_t, FontResource> fonts;
    std::unordered_map<std::uint64_t, sk_sp<SkImage>> images;
    std::uint64_t resource_epoch{1};

    ~Impl()
    {
        for (auto &[id, font] : fonts) {
            if (font.face) {
                FT_Done_Face(font.face);
            }
        }
        if (freetype) {
            FT_Done_FreeType(freetype);
        }
    }
};

namespace {
SkColor ToSkColor(contracts::Color color)
{
    return SkColorSetARGB(color.a, color.r, color.g, color.b);
}

SkPaint ColorPaint(contracts::Color color)
{
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(ToSkColor(color));
    return paint;
}

SkPaint StrokePaint(const contracts::StrokeRoundedRect &rect)
{
    auto paint = ColorPaint(rect.color);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(rect.width);
    return paint;
}

SkPaint ShadowPaint(const contracts::RoundedRectShadow &shadow)
{
    auto paint = ColorPaint(shadow.color);
    if (shadow.blur > 0) {
        paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, shadow.blur));
    }
    return paint;
}

SkRect ToSkRect(contracts::LogicalRect rect)
{
    return SkRect::MakeXYWH(static_cast<SkScalar>(rect.x), static_cast<SkScalar>(rect.y),
                            static_cast<SkScalar>(rect.width), static_cast<SkScalar>(rect.height));
}

SkRect IntegerHardClip(contracts::LogicalRect rect)
{
    const auto bounds = ToSkRect(rect);
    if (bounds.isEmpty()) {
        return SkRect::MakeEmpty();
    }
    SkIRect pixels;
    bounds.roundOut(&pixels);
    return SkRect::Make(pixels);
}

bool ValidRect(contracts::LogicalRect r)
{
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) &&
           std::isfinite(r.height) && r.width >= 0 && r.height >= 0 && std::abs(r.x) <= 1e6 &&
           std::abs(r.y) <= 1e6 && r.width <= 1e6 && r.height <= 1e6;
}

bool Validate(const contracts::DisplayList &list,
              const std::unordered_map<std::uint64_t, sk_sp<SkImage>> &images, const auto &fonts)
{
    if (!list.window || list.commands.size() > 100000) {
        return false;
    }
    std::vector<bool> stack;
    for (const auto &command : list.commands) {
        if (auto *rect = std::get_if<contracts::FillRect>(&command)) {
            if (!ValidRect(rect->bounds)) {
                return false;
            }
        } else if (auto *rect = std::get_if<contracts::FillRoundedRect>(&command)) {
            if (!ValidRect(rect->bounds) || !std::isfinite(rect->radius) || rect->radius < 0) {
                return false;
            }
        } else if (auto *rect = std::get_if<contracts::StrokeRoundedRect>(&command)) {
            if (!ValidRect(rect->bounds) || !std::isfinite(rect->radius) || rect->radius < 0 ||
                !std::isfinite(rect->width) || rect->width < 0 || rect->width > 512) {
                return false;
            }
        } else if (auto *shadow = std::get_if<contracts::RoundedRectShadow>(&command)) {
            if (!ValidRect(shadow->bounds) || !std::isfinite(shadow->radius) ||
                shadow->radius < 0 || !std::isfinite(shadow->blur) || shadow->blur < 0 ||
                shadow->blur > 512 || !std::isfinite(shadow->offset_y) ||
                std::abs(shadow->offset_y) > 16384) {
                return false;
            }
        } else if (auto *icon = std::get_if<contracts::DrawIcon>(&command)) {
            if (!ValidRect(icon->bounds) || icon->icon < contracts::VectorIcon::Grid ||
                icon->icon > contracts::VectorIcon::Error) {
                return false;
            }
        } else if (auto *image = std::get_if<contracts::DrawImage>(&command)) {
            if (!image->image || !ValidRect(image->destination)) {
                return false;
            }
            if (image->fit < contracts::ImageFit::Fill || image->fit > contracts::ImageFit::Cover) {
                return false;
            }
            if (!images.contains(image->image.value)) {
                return false;
            }
        } else if (auto *run = std::get_if<contracts::DrawGlyphRun>(&command)) {
            if (!run->font || !fonts.contains(run->font.value) || run->glyphs.size() > 100000 ||
                !std::isfinite(run->font_size) || run->font_size <= 0 || run->font_size > 512) {
                return false;
            }
            for (auto glyph : run->glyphs) {
                if (glyph.glyph_index > UINT16_MAX || !std::isfinite(glyph.origin.x) ||
                    !std::isfinite(glyph.origin.y)) {
                    return false;
                }
            }
        } else if (auto *clip = std::get_if<contracts::PushClipRect>(&command)) {
            if (!ValidRect(clip->bounds) || stack.size() >= 256) {
                return false;
            }
            stack.push_back(true);
        } else if (auto *clip = std::get_if<contracts::PushClipRoundedRect>(&command)) {
            if (!ValidRect(clip->bounds) || !std::isfinite(clip->radius) || clip->radius < 0 ||
                stack.size() >= 256) {
                return false;
            }
            stack.push_back(true);
        } else if (auto *transform = std::get_if<contracts::PushTransform>(&command)) {
            if (stack.size() >= 256) {
                return false;
            }
            for (double v : transform->values) {
                if (!std::isfinite(v) || std::abs(v) > 1e6) {
                    return false;
                }
            }
            stack.push_back(false);
        } else if (std::holds_alternative<contracts::PopClip>(command)) {
            if (stack.empty() || !stack.back()) {
                return false;
            }
            stack.pop_back();
        } else if (std::holds_alternative<contracts::PopTransform>(command)) {
            if (stack.empty() || stack.back()) {
                return false;
            }
            stack.pop_back();
        }
    }
    return stack.empty();
}

// The same SkPaint/SkFont configuration used by replay supplies ink bounds.
// Unknown bounds are never replaced with a guessed blur radius or text advance.
bool InkBounds(const contracts::DrawCommand &command, const auto &fonts, SkRect *ink)
{
    *ink = SkRect::MakeEmpty();
    SkRect raw;
    SkPaint paint;
    if (const auto *rect = std::get_if<contracts::FillRect>(&command)) {
        raw = ToSkRect(rect->bounds);
        paint = ColorPaint(rect->color);
    } else if (const auto *rect = std::get_if<contracts::FillRoundedRect>(&command)) {
        raw = ToSkRect(rect->bounds);
        paint = ColorPaint(rect->color);
    } else if (const auto *rect = std::get_if<contracts::StrokeRoundedRect>(&command)) {
        raw = ToSkRect(rect->bounds);
        raw.inset(rect->width / 2, rect->width / 2);
        raw = raw.makeSorted(); // SkRRect normalizes the same inset geometry.
        paint = StrokePaint(*rect);
    } else if (const auto *shadow = std::get_if<contracts::RoundedRectShadow>(&command)) {
        raw = ToSkRect(shadow->bounds);
        if (shadow->inset) {
            // Replay clips the entire inverse shadow to this rounded rectangle.
            *ink = raw;
            return ink->isFinite();
        }
        raw.offset(0, shadow->offset_y);
        paint = ShadowPaint(*shadow);
    } else if (const auto *icon = std::get_if<contracts::DrawIcon>(&command)) {
        return IconInkBounds(*icon, ink);
    } else if (const auto *image = std::get_if<contracts::DrawImage>(&command)) {
        *ink = ToSkRect(image->destination); // Also contains fitted image ink.
        return ink->isFinite();
    } else if (const auto *run = std::get_if<contracts::DrawGlyphRun>(&command)) {
        const SkFont font(fonts.at(run->font.value).typeface,
                          static_cast<SkScalar>(run->font_size));
        const auto text_paint = ColorPaint(run->color);
        std::vector<SkGlyphID> ids;
        std::vector<SkRect> bounds(run->glyphs.size());
        ids.reserve(run->glyphs.size());
        for (const auto &glyph : run->glyphs) {
            ids.push_back(static_cast<SkGlyphID>(glyph.glyph_index));
        }
        if (ids.empty()) {
            return true;
        }
        font.getBounds(ids.data(), static_cast<int>(ids.size()), bounds.data(), &text_paint);
        for (std::size_t i = 0; i < bounds.size(); ++i) {
            bounds[i].offset(static_cast<SkScalar>(run->glyphs[i].origin.x),
                             static_cast<SkScalar>(run->glyphs[i].origin.y));
            if (!bounds[i].isFinite()) {
                return false;
            }
            ink->join(bounds[i]);
        }
        return true;
    } else {
        return false;
    }
    if (!raw.isFinite() || !paint.canComputeFastBounds()) {
        return false;
    }
    SkRect storage;
    *ink = paint.computeFastBounds(raw, &storage);
    return ink->isFinite();
}
} // namespace

RasterRenderer::RasterRenderer(std::string font_path) : impl_(std::make_unique<Impl>())
{
    if (FT_Init_FreeType(&impl_->freetype) != 0) {
        return;
    }
    RegisterFont(default_font_, font_path);
}

RasterRenderer::~RasterRenderer() = default;

bool RasterRenderer::Ready() const
{
    return impl_->fonts.contains(default_font_.value);
}

bool RasterRenderer::RegisterFont(contracts::ResourceId id, const std::string &path)
{
    if (!id || !impl_->freetype || impl_->fonts.contains(id.value) ||
        impl_->resource_epoch == UINT64_MAX) {
        return false;
    }
    Impl::FontResource font;
    if (FT_New_Face(impl_->freetype, path.c_str(), 0, &font.face) != 0) {
        return false;
    }
    font.typeface = SkTypeface::MakeFromFile(path.c_str());
    if (!font.typeface) {
        FT_Done_Face(font.face);
        return false;
    }
    impl_->fonts.emplace(id.value, std::move(font));
    ++impl_->resource_epoch;
    return true;
}

std::optional<runtime::DecodedImage> RasterRenderer::DecodePng(const std::string &path)
{
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&png, path.c_str())) {
        return std::nullopt;
    }
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

bool RasterRenderer::RegisterImage(contracts::ResourceId id, const runtime::DecodedImage &image)
{
    if (!id || image.width == 0 || image.height == 0 || image.width > 4096 || image.height > 4096 ||
        image.rgba.size() != static_cast<std::size_t>(image.width) * image.height * 4 ||
        impl_->resource_epoch == UINT64_MAX) {
        return false;
    }
    auto info = SkImageInfo::Make(static_cast<int>(image.width), static_cast<int>(image.height),
                                  kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    SkPixmap pixmap(info, image.rgba.data(), static_cast<std::size_t>(image.width) * 4);
    auto sk_image = SkImages::RasterFromPixmapCopy(pixmap);
    if (!sk_image) {
        return false;
    }
    impl_->images[id.value] = std::move(sk_image);
    ++impl_->resource_epoch;
    return true;
}

std::uint64_t RasterRenderer::ResourceEpoch() const
{
    return impl_->resource_epoch;
}

std::optional<contracts::DamageRegion>
RasterRenderer::ClipRepair(const contracts::DamageRegion &repair, int width, int height)
{
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        return std::nullopt;
    }
    if (repair.full) {
        return contracts::DamageRegion::Full();
    }
    SkRegion union_region;
    for (const auto &rect : repair.rects) {
        if (rect.width < 0 || rect.height < 0) {
            return std::nullopt;
        }
        const auto left = std::clamp<std::int64_t>(rect.x, 0, width);
        const auto top = std::clamp<std::int64_t>(rect.y, 0, height);
        const auto right = std::clamp<std::int64_t>(std::int64_t(rect.x) + rect.width, 0, width);
        const auto bottom = std::clamp<std::int64_t>(std::int64_t(rect.y) + rect.height, 0, height);
        if (right <= left || bottom <= top) {
            continue;
        }
        union_region.op(SkIRect::MakeLTRB(static_cast<int>(left), static_cast<int>(top),
                                          static_cast<int>(right), static_cast<int>(bottom)),
                        SkRegion::kUnion_Op);
    }
    // Full is legal only when the declared union itself covers the target.
    if (union_region.contains(SkIRect::MakeWH(width, height))) {
        return contracts::DamageRegion::Full();
    }
    contracts::DamageRegion result;
    for (SkRegion::Iterator it(union_region); !it.done(); it.next()) {
        const auto &rect = it.rect();
        result.rects.push_back({rect.x(), rect.y(), rect.width(), rect.height()});
    }
    return result;
}

contracts::DamageRegion RasterRenderer::CompareDamage(const contracts::DisplayList *previous,
                                                      const contracts::DisplayList &next, int width,
                                                      int height,
                                                      std::uint64_t previous_resource_epoch) const
{
    if (!Ready() || width <= 0 || height <= 0 || width > 4096 || height > 4096 || !previous ||
        previous_resource_epoch != ResourceEpoch() || previous->window != next.window ||
        previous->commands.size() != next.commands.size() ||
        !Validate(*previous, impl_->images, impl_->fonts) ||
        !Validate(next, impl_->images, impl_->fonts)) {
        return contracts::DamageRegion::Full();
    }
    contracts::DamageRegion damage;
    SkRect clip = SkRect::MakeWH(width, height);
    std::vector<SkRect> clips;
    for (std::size_t i = 0; i < next.commands.size(); ++i) {
        const auto &old = previous->commands[i];
        const auto &current = next.commands[i];
        if (old.index() != current.index()) {
            return contracts::DamageRegion::Full();
        }
        if (const auto *push = std::get_if<contracts::PushClipRect>(&current)) {
            if (old != current) {
                return contracts::DamageRegion::Full();
            }
            clips.push_back(clip);
            if (!clip.intersect(IntegerHardClip(push->bounds))) {
                clip.setEmpty();
            }
        } else if (const auto *push = std::get_if<contracts::PushClipRoundedRect>(&current)) {
            if (old != current) {
                return contracts::DamageRegion::Full();
            }
            clips.push_back(clip);
            auto envelope = ToSkRect(push->bounds);
            envelope.outset(2, 2); // Rounded clip uses antialiasing in replay.
            if (!clip.intersect(envelope)) {
                clip.setEmpty();
            }
        } else if (const auto *push = std::get_if<contracts::PushTransform>(&current)) {
            constexpr std::array<double, 6> identity{1, 0, 0, 0, 1, 0};
            if (old != current || push->values != identity) {
                return contracts::DamageRegion::Full();
            }
            clips.push_back(clip);
        } else if (std::holds_alternative<contracts::PopClip>(current) ||
                   std::holds_alternative<contracts::PopTransform>(current)) {
            clip = clips.back();
            clips.pop_back();
        } else if (old != current && !clip.isEmpty()) {
            for (const auto *command : {&old, &current}) {
                SkRect bounds;
                if (!InkBounds(*command, impl_->fonts, &bounds)) {
                    return contracts::DamageRegion::Full();
                }
                if (bounds.isEmpty()) {
                    continue;
                }
                // Device AA/text hinting may touch adjacent pixels beyond the
                // font's geometric ink. Both old and new masks receive a guard.
                bounds.outset(2, 2);
                if (!bounds.intersect(clip)) {
                    continue;
                }
                SkIRect rounded;
                bounds.roundOut(&rounded);
                damage.rects.push_back(
                    {rounded.x(), rounded.y(), rounded.width(), rounded.height()});
            }
        }
    }
    return runtime::NormalizeDamage(
        damage, {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)});
}

runtime::ShapedText RasterRenderer::Shape(std::string_view text, double size) const
{
    return Shape(default_font_, text, size);
}

runtime::ShapedText RasterRenderer::Shape(contracts::ResourceId id, std::string_view text,
                                          double size) const
{
    runtime::ShapedText result;
    auto entry = impl_->fonts.find(id.value);
    if (entry == impl_->fonts.end() || !std::isfinite(size) || size <= 0 || size > 512) {
        return result;
    }
    if (FT_Set_Char_Size(entry->second.face, 0, static_cast<FT_F26Dot6>(size * 64), 0, 0) != 0) {
        return result;
    }
    hb_font_t *font = hb_ft_font_create_referenced(entry->second.face);
    if (!font) {
        return result;
    }
    hb_buffer_t *buffer = hb_buffer_create();
    hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()), 0,
                       static_cast<int>(text.size()));
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(font, buffer, nullptr, 0);
    unsigned count = 0;
    auto *infos = hb_buffer_get_glyph_infos(buffer, &count);
    auto *positions = hb_buffer_get_glyph_positions(buffer, &count);
    const double ascent = entry->second.face->size->metrics.ascender / 64.0;
    const double descent = -entry->second.face->size->metrics.descender / 64.0;
    double cursor_x = 0, cursor_y = ascent;
    result.glyphs.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        result.glyphs.push_back(
            {infos[i].codepoint,
             {cursor_x + positions[i].x_offset / 64.0, cursor_y - positions[i].y_offset / 64.0}});
        cursor_x += positions[i].x_advance / 64.0;
        cursor_y -= positions[i].y_advance / 64.0;
    }
    result.width = std::max(0.0, cursor_x);
    result.height = ascent + descent;
    hb_buffer_destroy(buffer);
    hb_font_destroy(font);
    return result;
}

bool RasterRenderer::Render(const contracts::DisplayList &list, void *pixels, int width, int height,
                            int stride) const
{
    return Render(list, pixels, width, height, stride, contracts::DamageRegion::Full());
}

bool RasterRenderer::Render(const contracts::DisplayList &list, void *pixels, int width, int height,
                            int stride, const contracts::DamageRegion &repair) const
{
    if (!pixels || width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
        stride < width * 4) {
        return false;
    }
    const auto clipped = ClipRepair(repair, width, height);
    if (!clipped) {
        return false;
    }
    if (!clipped->full) {
        // Raster scan conversion may clip curves before computing AA coverage,
        // changing a few rounding bits compared with an unclipped full replay.
        // This diagnostic backend conservatively rasterizes the complete list
        // into scratch, then changes only the exactly declared repair spans.
        // Ganesh calls Replay directly and retains its actual partial drawing.
        if (clipped->rects.empty()) {
            return Ready() && Validate(list, impl_->images, impl_->fonts);
        }
        std::vector<std::uint8_t> scratch(static_cast<std::size_t>(width) * height * 4);
        if (!Render(list, scratch.data(), width, height, width * 4)) {
            return false;
        }
        auto *destination = static_cast<std::uint8_t *>(pixels);
        for (const auto &rect : clipped->rects) {
            const auto span_bytes = static_cast<std::size_t>(rect.width) * 4;
            for (int y = rect.y; y < rect.y + rect.height; ++y) {
                const auto x_bytes = static_cast<std::size_t>(rect.x) * 4;
                std::memcpy(destination + static_cast<std::size_t>(y) * stride + x_bytes,
                            scratch.data() + static_cast<std::size_t>(y) * width * 4 + x_bytes,
                            span_bytes);
            }
        }
        return true;
    }
    const auto info = SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
    auto surface = SkSurfaces::WrapPixels(info, pixels, static_cast<std::size_t>(stride));
    if (!surface) {
        return false;
    }
    return Replay(list, surface->getCanvas(), width, height, repair);
}

bool RasterRenderer::Replay(const contracts::DisplayList &list, SkCanvas *canvas) const
{
    if (!canvas) {
        return false;
    }
    const auto size = canvas->getBaseLayerSize();
    return Replay(list, canvas, size.width(), size.height(), contracts::DamageRegion::Full());
}

bool RasterRenderer::Replay(const contracts::DisplayList &list, SkCanvas *canvas, int width,
                            int height, const contracts::DamageRegion &repair) const
{
    if (!Ready() || !canvas || !Validate(list, impl_->images, impl_->fonts)) {
        return false;
    }
    const auto clipped = ClipRepair(repair, width, height);
    if (!clipped) {
        return false;
    }
    if (!clipped->full && clipped->rects.empty()) {
        return true;
    }
    SkAutoCanvasRestore restore(canvas, true);
    if (!clipped->full) {
        SkRegion region;
        for (const auto &rect : clipped->rects) {
            region.op(SkIRect::MakeXYWH(rect.x, rect.y, rect.width, rect.height),
                      SkRegion::kUnion_Op);
        }
        canvas->clipRegion(region, SkClipOp::kIntersect);
    }
    // Src replacement erases old translucent pixels only in the repair clip.
    canvas->drawColor(SK_ColorTRANSPARENT, SkBlendMode::kSrc);
    for (const auto &command : list.commands) {
        if (auto *rect = std::get_if<contracts::FillRect>(&command)) {
            auto paint = ColorPaint(rect->color);
            canvas->drawRect(ToSkRect(rect->bounds), paint);
        } else if (auto *rect = std::get_if<contracts::FillRoundedRect>(&command)) {
            auto paint = ColorPaint(rect->color);
            SkRRect rounded;
            rounded.setRectXY(ToSkRect(rect->bounds), static_cast<SkScalar>(rect->radius),
                              static_cast<SkScalar>(rect->radius));
            canvas->drawRRect(rounded, paint);
        } else if (auto *rect = std::get_if<contracts::StrokeRoundedRect>(&command)) {
            auto paint = StrokePaint(*rect);
            auto bounds = ToSkRect(rect->bounds);
            bounds.inset(rect->width / 2, rect->width / 2);
            SkRRect rounded;
            rounded.setRectXY(bounds, std::max(0.0, rect->radius - rect->width / 2),
                              std::max(0.0, rect->radius - rect->width / 2));
            canvas->drawRRect(rounded, paint);
        } else if (auto *shadow = std::get_if<contracts::RoundedRectShadow>(&command)) {
            auto paint = ShadowPaint(*shadow);
            auto bounds = ToSkRect(shadow->bounds);
            SkRRect rounded;
            rounded.setRectXY(bounds, shadow->radius, shadow->radius);
            if (shadow->inset) {
                canvas->save();
                canvas->clipRRect(rounded, true);
                SkPath outside;
                auto expanded = bounds;
                expanded.outset(3 * shadow->blur + std::abs(shadow->offset_y) + 4,
                                3 * shadow->blur + std::abs(shadow->offset_y) + 4);
                outside.addRect(expanded);
                bounds.offset(0, shadow->offset_y);
                rounded.setRectXY(bounds, shadow->radius, shadow->radius);
                outside.addRRect(rounded);
                outside.setFillType(SkPathFillType::kEvenOdd);
                canvas->drawPath(outside, paint);
                canvas->restore();
            } else {
                bounds.offset(0, shadow->offset_y);
                rounded.setRectXY(bounds, shadow->radius, shadow->radius);
                canvas->drawRRect(rounded, paint);
            }
        } else if (auto *icon = std::get_if<contracts::DrawIcon>(&command)) {
            ReplayIcon(canvas, *icon);
        } else if (auto *run = std::get_if<contracts::DrawGlyphRun>(&command)) {
            SkFont font(impl_->fonts.at(run->font.value).typeface,
                        static_cast<SkScalar>(run->font_size));
            auto paint = ColorPaint(run->color);
            std::vector<SkGlyphID> glyphs;
            std::vector<SkPoint> points;
            glyphs.reserve(run->glyphs.size());
            points.reserve(run->glyphs.size());
            for (const auto &glyph : run->glyphs) {
                glyphs.push_back(static_cast<SkGlyphID>(glyph.glyph_index));
                points.push_back(SkPoint::Make(static_cast<SkScalar>(glyph.origin.x),
                                               static_cast<SkScalar>(glyph.origin.y)));
            }
            if (!glyphs.empty()) {
                canvas->drawGlyphs(static_cast<int>(glyphs.size()), glyphs.data(), points.data(),
                                   SkPoint::Make(0, 0), font, paint);
            }
        } else if (auto *image = std::get_if<contracts::DrawImage>(&command)) {
            auto resource = impl_->images.at(image->image.value);
            auto destination = ToSkRect(image->destination);
            if (destination.isEmpty()) {
                continue;
            }
            if (image->fit == contracts::ImageFit::Contain) {
                const double scale = std::min(destination.width() / resource->width(),
                                              destination.height() / resource->height());
                const auto width = resource->width() * scale, height = resource->height() * scale;
                destination = SkRect::MakeXYWH(
                    destination.x() + (destination.width() - width) / 2,
                    destination.y() + (destination.height() - height) / 2, width, height);
            }
            if (image->fit == contracts::ImageFit::Cover) {
                if (destination.width() == 0 || destination.height() == 0) {
                    continue;
                }
                const double scale = std::max(destination.width() / resource->width(),
                                              destination.height() / resource->height());
                const auto width = destination.width() / scale,
                           height = destination.height() / scale;
                const auto source =
                    SkRect::MakeXYWH((resource->width() - width) / 2,
                                     (resource->height() - height) / 2, width, height);
                canvas->drawImageRect(resource, source, destination,
                                      SkSamplingOptions(SkFilterMode::kLinear), nullptr,
                                      SkCanvas::kStrict_SrcRectConstraint);
            } else {
                canvas->drawImageRect(resource, destination,
                                      SkSamplingOptions(SkFilterMode::kLinear), nullptr);
            }
        } else if (auto *clip = std::get_if<contracts::PushClipRect>(&command)) {
            canvas->save();
            // A fractional non-AA rect can switch Ganesh coverage algorithms
            // when the repair clip narrows its remaining extent below 1px.
            // Define identity hard clips as outward integer pixel coverage so
            // full and partial replay use the same scissor boundaries. Other
            // transforms retain their geometry and require full damage.
            canvas->clipRect(canvas->getTotalMatrix().isIdentity() ? IntegerHardClip(clip->bounds)
                                                                   : ToSkRect(clip->bounds));
        } else if (auto *clip = std::get_if<contracts::PushClipRoundedRect>(&command)) {
            canvas->save();
            SkRRect rounded;
            rounded.setRectXY(ToSkRect(clip->bounds), clip->radius, clip->radius);
            canvas->clipRRect(rounded, true);
        } else if (auto *transform = std::get_if<contracts::PushTransform>(&command)) {
            canvas->save();
            const auto &v = transform->values;
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
