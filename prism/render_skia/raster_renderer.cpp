#include "prism/render_skia/raster_renderer.hpp"
#include "contour_renderer_p.hpp"
#include "image_provider_p.hpp"
#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRegion.h"
#include "include/core/SkSurface.h"
#include "opacity_layers_p.hpp"
#include "prism/contracts/display_list_validation.hpp"
#include "render_geometry_p.hpp"
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
using detail::ColorPaint;
using detail::ShadowPaint;
using detail::StrokePaint;
using detail::ToSkRect;

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
    return detail::CompareDisplayListDamage(*previous, next, width, height, impl_->resources);
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
    const auto layer_hints = detail::OpacityLayerHints(list, resources);
    std::size_t layer_index = 0;
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
        } else if (auto *fill = std::get_if<contracts::FillContour>(&command)) {
            detail::ReplayContour(canvas, *fill);
        } else if (auto *stroke = std::get_if<contracts::StrokeContour>(&command)) {
            detail::ReplayContour(canvas, *stroke);
        } else if (auto *shadow = std::get_if<contracts::ContourShadow>(&command)) {
            detail::ReplayContour(canvas, *shadow);
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
            // Rect-preserving transforms share outward device pixel coverage
            // in full and partial replay, including positive scale/translation.
            const auto &matrix = canvas->getTotalMatrix();
            if (matrix.rectStaysRect()) {
                const auto bounds = detail::HardClipBounds(clip->bounds, matrix);
                SkIRect pixels;
                bounds.roundOut(&pixels);
                canvas->clipRegion(SkRegion(pixels));
            } else {
                canvas->clipRect(ToSkRect(clip->bounds));
            }
        } else if (auto *clip = std::get_if<contracts::PushClipRoundedRect>(&command)) {
            canvas->save();
            SkRRect rounded;
            rounded.setRectXY(ToSkRect(clip->bounds), clip->radius, clip->radius);
            canvas->clipRRect(rounded, true);
        } else if (auto *clip = std::get_if<contracts::PushClipContour>(&command)) {
            detail::ReplayContourClip(canvas, *clip);
        } else if (auto *transform = std::get_if<contracts::PushTransform>(&command)) {
            canvas->save();
            canvas->concat(detail::TransformMatrix(*transform));
        } else if (auto *opacity = std::get_if<contracts::PushOpacity>(&command)) {
            if (opacity->opacity > 0 && opacity->opacity < 1) {
                const auto &hint = layer_hints[layer_index];
                canvas->saveLayerAlphaf(hint ? &*hint : nullptr, opacity->opacity);
            } else {
                canvas->save();
                if (opacity->opacity == 0) {
                    canvas->clipRect(SkRect::MakeEmpty());
                }
            }
            ++layer_index;
        } else if (std::holds_alternative<contracts::PopClip>(command) ||
                   std::holds_alternative<contracts::PopTransform>(command) ||
                   std::holds_alternative<contracts::PopOpacity>(command)) {
            canvas->restore();
        }
    }
    return true;
}

} // namespace prism::render_skia
