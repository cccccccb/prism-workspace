#pragma once

#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "prism/contracts/display_list.hpp"

class SkCanvas;

namespace prism::render_skia::detail {

SkPath ToSkContour(const contracts::Contour &contour);
SkPaint ContourStrokePaint(const contracts::StrokeContour &stroke);
SkPaint ContourShadowPaint(const contracts::ContourShadow &shadow);
void ReplayContour(SkCanvas *canvas, const contracts::FillContour &fill);
void ReplayContour(SkCanvas *canvas, const contracts::StrokeContour &stroke);
void ReplayContour(SkCanvas *canvas, const contracts::ContourShadow &shadow);
void ReplayContourClip(SkCanvas *canvas, const contracts::PushClipContour &clip);

} // namespace prism::render_skia::detail
