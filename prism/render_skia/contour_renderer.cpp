#include "contour_renderer_p.hpp"

#include "include/core/SkBlurTypes.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkMaskFilter.h"
#include "render_geometry_p.hpp"
#include <cmath>

namespace prism::render_skia::detail {

SkPath ToSkContour(const contracts::Contour &contour)
{
    SkPath path;
    path.setFillType(SkPathFillType::kEvenOdd);
    path.moveTo(contour.points.front().x, contour.points.front().y);
    for (std::size_t index = 1; index < contour.points.size(); ++index) {
        path.lineTo(contour.points[index].x, contour.points[index].y);
    }
    path.close();
    return path;
}

SkPaint ContourStrokePaint(const contracts::StrokeContour &stroke)
{
    auto paint = ColorPaint(stroke.color);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(2 * stroke.width);
    paint.setStrokeJoin(SkPaint::kRound_Join);
    return paint;
}

SkPaint ContourShadowPaint(const contracts::ContourShadow &shadow)
{
    auto paint = ColorPaint(shadow.color);
    if (shadow.blur > 0) {
        paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, shadow.blur));
    }
    return paint;
}

void ReplayContour(SkCanvas *canvas, const contracts::FillContour &fill)
{
    canvas->drawPath(ToSkContour(fill.contour), ColorPaint(fill.color));
}

void ReplayContour(SkCanvas *canvas, const contracts::StrokeContour &stroke)
{
    if (stroke.width == 0) {
        return;
    }

    const auto path = ToSkContour(stroke.contour);
    SkAutoCanvasRestore restore(canvas, true);
    canvas->clipPath(path, SkClipOp::kIntersect, true);
    canvas->drawPath(path, ContourStrokePaint(stroke));
}

void ReplayContour(SkCanvas *canvas, const contracts::ContourShadow &shadow)
{
    auto path = ToSkContour(shadow.contour);
    const auto paint = ContourShadowPaint(shadow);
    if (!shadow.inset) {
        path.offset(0, shadow.offset_y);
        canvas->drawPath(path, paint);
        return;
    }

    SkAutoCanvasRestore restore(canvas, true);
    canvas->clipPath(path, SkClipOp::kIntersect, true);

    auto expanded = ToSkRect(contracts::ContourBounds(shadow.contour));
    const auto margin = 3 * shadow.blur + std::abs(shadow.offset_y) + 4;
    expanded.outset(margin, margin);
    path.offset(0, shadow.offset_y);

    SkPath outside;
    outside.addRect(expanded);
    outside.addPath(path);
    outside.setFillType(SkPathFillType::kEvenOdd);
    canvas->drawPath(outside, paint);
}

void ReplayContourClip(SkCanvas *canvas, const contracts::PushClipContour &clip)
{
    canvas->save();
    canvas->clipPath(ToSkContour(clip.contour), SkClipOp::kIntersect, true);
}

} // namespace prism::render_skia::detail
