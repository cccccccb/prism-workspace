#include "render_geometry_p.hpp"

#include "contour_renderer_p.hpp"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkFont.h"
#include "include/core/SkMaskFilter.h"
#include "resource_table_p.hpp"
#include "vector_icons.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace prism::render_skia::detail {
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

// The same SkPaint/SkFont configuration used by replay supplies ink bounds.
// Unknown bounds are never replaced with a guessed blur radius or text advance.
bool InkBounds(const contracts::DrawCommand &command, const detail::ResourceTable &resources,
               SkRect *ink)
{
    *ink = SkRect::MakeEmpty();
    const bool transparent = std::visit(
        [](const auto &value) {
            if constexpr (requires { value.color.a; }) {
                return value.color.a == 0;
            }
            return false;
        },
        command);
    if (transparent) {
        return true;
    }

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
    } else if (const auto *fill = std::get_if<contracts::FillContour>(&command)) {
        raw = ToSkRect(contracts::ContourBounds(fill->contour));
        paint = ColorPaint(fill->color);
    } else if (const auto *stroke = std::get_if<contracts::StrokeContour>(&command)) {
        if (stroke->width > 0) {
            // Replay clips the doubled centered stroke to the canonical contour.
            *ink = ToSkRect(contracts::ContourBounds(stroke->contour));
        }
        return ink->isFinite();
    } else if (const auto *shadow = std::get_if<contracts::ContourShadow>(&command)) {
        raw = ToSkRect(contracts::ContourBounds(shadow->contour));
        if (shadow->inset) {
            *ink = raw;
            return ink->isFinite();
        }
        raw.offset(0, shadow->offset_y);
        paint = ContourShadowPaint(*shadow);
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

SkMatrix TransformMatrix(const contracts::PushTransform &transform)
{
    const auto &values = transform.values;
    SkMatrix matrix;
    matrix.setAll(values[0], values[1], values[2], values[3], values[4], values[5], 0, 0, 1);
    return matrix;
}

bool DeviceInkBounds(const contracts::DrawCommand &command, const ResourceTable &resources,
                     const SkMatrix &matrix, SkRect *ink)
{
    SkRect local;
    if (!InkBounds(command, resources, &local)) {
        return false;
    }
    if (local.isEmpty()) {
        *ink = SkRect::MakeEmpty();
        return true;
    }
    *ink = matrix.mapRect(local);

    SkRect raw;
    bool outer_blur = false;
    if (const auto *shadow = std::get_if<contracts::RoundedRectShadow>(&command);
        shadow && !shadow->inset && shadow->blur > 0) {
        raw = ToSkRect(shadow->bounds);
        raw.offset(0, shadow->offset_y);
        outer_blur = true;
    } else if (const auto *shadow = std::get_if<contracts::ContourShadow>(&command);
               shadow && !shadow->inset && shadow->blur > 0) {
        raw = ToSkRect(contracts::ContourBounds(shadow->contour));
        raw.offset(0, shadow->offset_y);
        outer_blur = true;
    }
    if (outer_blur) {
        // Skia's mask blur maps sigma radially. Mapping the local expanded
        // rectangle alone can underestimate the smaller axis of a nonuniform
        // scale. Derive the margin from the same paint, then bound its device
        // radius by the matrix's maximum scale; no guessed blur cutoff.
        const auto margin = std::max({raw.left() - local.left(), raw.top() - local.top(),
                                      local.right() - raw.right(), local.bottom() - raw.bottom()});
        const auto scale = matrix.getMaxScale();
        if (!std::isfinite(scale) || scale < 0 || !std::isfinite(margin * scale)) {
            return false;
        }
        raw = matrix.mapRect(raw);
        raw.outset(margin * scale, margin * scale);
        ink->join(raw);
    }
    return ink->isFinite();
}

SkRect HardClipBounds(contracts::LogicalRect bounds, const SkMatrix &matrix)
{
    auto result = matrix.mapRect(ToSkRect(bounds));
    if (matrix.rectStaysRect() && !result.isEmpty()) {
        SkIRect pixels;
        result.roundOut(&pixels);
        result = SkRect::Make(pixels);
    }
    return result;
}
} // namespace prism::render_skia::detail
