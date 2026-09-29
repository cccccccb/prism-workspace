#pragma once
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"
#include "prism/contracts/damage.hpp"
#include "prism/contracts/display_list.hpp"

namespace prism::render_skia::detail {
class ResourceTable;

SkPaint ColorPaint(contracts::Color color);
SkPaint StrokePaint(const contracts::StrokeRoundedRect &rect);
SkPaint ShadowPaint(const contracts::RoundedRectShadow &shadow);
SkRect ToSkRect(contracts::LogicalRect rect);
SkMatrix TransformMatrix(const contracts::PushTransform &transform);
SkRect HardClipBounds(contracts::LogicalRect bounds, const SkMatrix &matrix);
bool InkBounds(const contracts::DrawCommand &command, const ResourceTable &resources, SkRect *ink);
bool DeviceInkBounds(const contracts::DrawCommand &command, const ResourceTable &resources,
                     const SkMatrix &matrix, SkRect *ink);

contracts::DamageRegion CompareDisplayListDamage(const contracts::DisplayList &previous,
                                                 const contracts::DisplayList &next, int width,
                                                 int height, const ResourceTable &resources);
} // namespace prism::render_skia::detail
