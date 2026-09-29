#include "prism/render_skia/raster_renderer.hpp"
#include "image_provider_p.hpp"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRegion.h"
#include "include/core/SkSurface.h"
#include "prism/contracts/display_list_validation.hpp"
#include "prism/runtime/buffer_damage.hpp"
#include "resource_table_p.hpp"
#include "vector_icons.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <variant>
#include <vector>

namespace prism::render_skia {

struct RasterRenderer::Impl {
    detail::ResourceTable resources;
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

bool Validate(const contracts::DisplayList &list, const detail::ResourceTable &resources)
{
    try {
        contracts::ValidateDisplayList(list);
    } catch (const std::invalid_argument &) {
        return false;
    }

    for (const auto &command : list.commands) {
        if (const auto *image = std::get_if<contracts::DrawImage>(&command)) {
            if (!image->image || !resources.HasImage(image->image)) {
                return false;
            }
        } else if (const auto *run = std::get_if<contracts::DrawGlyphRun>(&command)) {
            if (!run->font || !resources.HasFont(run->font)) {
                return false;
            }
        }
    }
    return true;
}

// The same SkPaint/SkFont configuration used by replay supplies ink bounds.
// Unknown bounds are never replaced with a guessed blur radius or text advance.
bool InkBounds(const contracts::DrawCommand &command, const detail::ResourceTable &resources,
               SkRect *ink)
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
        const SkFont font(resources.Font(run->font), static_cast<SkScalar>(run->font_size));
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
    RegisterFont(default_font_, font_path);
}

RasterRenderer::~RasterRenderer() = default;

bool RasterRenderer::Ready() const
{
    return impl_->resources.HasFont(default_font_);
}

bool RasterRenderer::RegisterFont(contracts::ResourceId id, const std::string &path)
{
    return impl_->resources.RegisterFont(id, path);
}

bool RasterRenderer::RegisterImage(contracts::ResourceId id, const runtime::DecodedImage &image)
{
    return impl_->resources.RegisterImage(id, image);
}

bool RasterRenderer::RegisterImage(contracts::ResourceId id, runtime::ImageLease image)
{
    return impl_->resources.RegisterImage(id, std::move(image));
}

void RasterRenderer::UnregisterImage(contracts::ResourceId id)
{
    impl_->resources.UnregisterImage(id);
}

std::uint64_t RasterRenderer::ResourceEpoch() const
{
    return impl_->resources.ResourceEpoch();
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
        !Validate(*previous, impl_->resources) || !Validate(next, impl_->resources)) {
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
                if (!InkBounds(*command, impl_->resources, &bounds)) {
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
            return Ready() && Validate(list, impl_->resources);
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
    return Replay(list, surface->getCanvas(), width, height, repair, impl_->resources);
}

bool RasterRenderer::Replay(const contracts::DisplayList &list, SkCanvas *canvas, int width,
                            int height, const contracts::DamageRegion &repair,
                            const detail::ResourceTable &resources,
                            const detail::ImageProvider *images)
{
    if (!resources.HasFont(contracts::ResourceId{1}) || !canvas || !Validate(list, resources)) {
        return false;
    }
    const auto clipped = ClipRepair(repair, width, height);
    if (!clipped) {
        return false;
    }
    if (!clipped->full && clipped->rects.empty()) {
        return true;
    }
    if (images) {
        for (const auto &command : list.commands) {
            const auto *image = std::get_if<contracts::DrawImage>(&command);
            if (image && !images->Find(image->image)) {
                return false;
            }
        }
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
            SkFont font(resources.Font(run->font), static_cast<SkScalar>(run->font_size));
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
            const auto *resource =
                images ? images->Find(image->image) : resources.Image(image->image);
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
