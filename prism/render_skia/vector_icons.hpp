#pragma once
#include "include/core/SkCanvas.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkRRect.h"
#include "prism/contracts/display_list.hpp"
#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace prism::render_skia {
// A shared 24-unit vector resource set. No text glyph or application-specific
// drawing branches enter the Scene; icons replay in raster and GLES alike.
class VectorIconWalker {
public:
    VectorIconWalker(SkCanvas *target, SkRect *bounds) : canvas(target), ink(bounds)
    {
    }

    bool Walk(const contracts::DrawIcon &icon)
    {
        if (canvas) {
            canvas->save();
        }
        const auto side = std::min(icon.bounds.width, icon.bounds.height);
        if (canvas) {
            canvas->translate(icon.bounds.x + (icon.bounds.width - side) / 2,
                              icon.bounds.y + (icon.bounds.height - side) / 2);
        }
        if (canvas) {
            canvas->scale(side / 24, side / 24);
        }

        stroke = SkPaint();
        stroke.setAntiAlias(true);
        stroke.setStyle(SkPaint::kStroke_Style);
        stroke.setStrokeWidth(1.7);
        stroke.setStrokeCap(SkPaint::kRound_Cap);
        stroke.setStrokeJoin(SkPaint::kRound_Join);
        stroke.setColor(SkColorSetARGB(icon.color.a, icon.color.r, icon.color.g, icon.color.b));
        fill = stroke;
        fill.setStyle(SkPaint::kFill_Style);
        local_ink = SkRect::MakeEmpty();
        valid = true;

        using I = contracts::VectorIcon;
        switch (icon.icon) {
        case I::Document:
        case I::DocumentAdd:
            path({{14, 3}, {5, 3}, {5, 21}, {19, 21}, {19, 8}, {14, 3}});
            path({{14, 3}, {14, 8}, {19, 8}});
            line(8, 12, 15, 12);
            if (icon.icon == I::DocumentAdd) {
                line(11.5, 9, 11.5, 15);
            } else {
                line(8, 16, 14, 16);
            }
            break;
        case I::Save:
            path({{4, 3}, {17, 3}, {21, 7}, {21, 21}, {3, 21}, {3, 3}, {4, 3}});
            path({{7, 3}, {7, 9}, {16, 9}, {16, 3}});
            path({{7, 21}, {7, 14}, {17, 14}, {17, 21}});
            line(13, 5, 13, 7);
            break;
        case I::Close:
            line(6, 6, 18, 18);
            line(18, 6, 6, 18);
            break;
        case I::ArrowLeft:
            path({{14, 5}, {7, 12}, {14, 19}});
            break;
        case I::ArrowRight:
            path({{10, 5}, {17, 12}, {10, 19}});
            break;
        case I::Trash:
            line(4, 6, 20, 6);
            path({{9, 6}, {9, 3}, {15, 3}, {15, 6}});
            path({{6, 6}, {7, 21}, {17, 21}, {18, 6}});
            line(10, 10, 10, 17);
            line(14, 10, 14, 17);
            break;
        case I::Info:
            drawOval(SkRect::MakeXYWH(3, 3, 18, 18), stroke);
            line(12, 11, 12, 17);
            drawOval(SkRect::MakeXYWH(11.2, 6.5, 1.6, 1.6), fill);
            break;
        case I::Fullscreen:
            path({{9, 4}, {4, 4}, {4, 9}});
            path({{15, 4}, {20, 4}, {20, 9}});
            path({{4, 15}, {4, 20}, {9, 20}});
            path({{15, 20}, {20, 20}, {20, 15}});
            break;
        case I::Restore:
            rect(7, 4, 13, 13, 2);
            rect(4, 7, 13, 13, 2);
            break;
        case I::SplitHorizontal:
            rect(3, 4, 18, 16, 2);
            path({{12, 4}, {12, 20}});
            break;
        case I::SplitVertical:
            rect(3, 4, 18, 16, 2);
            path({{3, 12}, {21, 12}});
            break;
        case I::Grid:
            for (int y : {4, 13}) {
                for (int x : {4, 13}) {
                    rect(x, y, 7, 7, 1.7, true);
                }
            }
            break;
        case I::Music:
            line(10, 17, 10, 5);
            line(10, 5, 20, 3);
            line(20, 3, 20, 15);
            line(10, 9, 20, 7);
            drawOval(SkRect::MakeXYWH(4, 15, 6, 5), fill);
            drawOval(SkRect::MakeXYWH(14, 13, 6, 5), fill);
            break;
        case I::Settings: {
            for (int i = 0; i < 8; ++i) {
                const float a = i * 3.14159265f / 4;
                line(12 + 7 * std::cos(a), 12 + 7 * std::sin(a), 12 + 10 * std::cos(a),
                     12 + 10 * std::sin(a));
            }
            drawCircle(12, 12, 7, stroke);
            drawCircle(12, 12, 3, stroke);
            break;
        }
        case I::Folder:
            path({{3, 7}, {3, 5}, {9, 5}, {11, 7}, {21, 7}, {21, 20}, {3, 20}}, true);
            line(3, 10, 21, 10);
            break;
        case I::Terminal:
            rect(2, 4, 20, 16, 3);
            path({{6, 9}, {9, 12}, {6, 15}});
            line(12, 16, 18, 16);
            break;
        case I::Play:
            path({{7, 4}, {20, 12}, {7, 20}}, true, true);
            break;
        case I::Pause:
            rect(5, 4, 5, 16, 1, true);
            rect(14, 4, 5, 16, 1, true);
            break;
        case I::Previous:
            rect(4, 5, 2, 14, 1, true);
            path({{19, 5}, {8, 12}, {19, 19}}, true, true);
            break;
        case I::Next:
            rect(18, 5, 2, 14, 1, true);
            path({{5, 5}, {16, 12}, {5, 19}}, true, true);
            break;
        case I::Volume:
            path({{3, 9}, {7, 9}, {12, 5}, {12, 19}, {7, 15}, {3, 15}}, true);
            {
                SkPath p;
                p.moveTo(16, 8);
                p.cubicTo(20, 9, 20, 15, 16, 16);
                drawPath(p, stroke);
                SkPath q;
                q.moveTo(19, 5);
                q.cubicTo(25, 8, 25, 16, 19, 19);
                drawPath(q, stroke);
            }
            break;
        case I::Wifi: {
            SkPath p;
            p.moveTo(3, 9);
            p.quadTo(12, 1, 21, 9);
            drawPath(p, stroke);
            SkPath q;
            q.moveTo(6, 13);
            q.quadTo(12, 7, 18, 13);
            drawPath(q, stroke);
            SkPath r;
            r.moveTo(9, 16);
            r.quadTo(12, 13, 15, 16);
            drawPath(r, stroke);
            drawCircle(12, 20, 1.4, fill);
        } break;
        case I::Battery:
            rect(2, 7, 18, 10, 2);
            rect(5, 10, 12, 4, 1, true);
            rect(21, 10, 2, 4, 1, true);
            break;
        case I::Search:
            drawCircle(10, 10, 6, stroke);
            line(14.5, 14.5, 21, 21);
            break;
        case I::Sun:
            drawCircle(12, 12, 4, stroke);
            for (int i = 0; i < 8; ++i) {
                const float a = i * 3.14159265f / 4;
                line(12 + 7 * std::cos(a), 12 + 7 * std::sin(a), 12 + 10 * std::cos(a),
                     12 + 10 * std::sin(a));
            }
            break;
        case I::Moon: {
            SkPath p;
            p.moveTo(18, 3);
            p.cubicTo(6, 1, 1, 14, 9, 20);
            p.cubicTo(15, 24, 23, 19, 22, 12);
            p.cubicTo(14, 16, 10, 7, 18, 3);
            p.close();
            drawPath(p, fill);
        } break;
        case I::Power: {
            SkPath p;
            p.moveTo(7, 5);
            p.cubicTo(-1, 11, 4, 22, 12, 22);
            p.cubicTo(20, 22, 25, 11, 17, 5);
            drawPath(p, stroke);
        }
            line(12, 2, 12, 12);
            break;
        case I::Check:
            path({{4, 12}, {10, 18}, {21, 5}});
            break;
        case I::Chevron:
            path({{9, 5}, {16, 12}, {9, 19}});
            break;
        case I::Refresh: {
            SkPath p;
            p.moveTo(20, 10);
            p.cubicTo(17, -1, 2, 2, 3, 13);
            p.cubicTo(4, 23, 18, 24, 21, 15);
            drawPath(p, stroke);
        }
            path({{20, 3}, {20, 10}, {13, 10}});
            break;
        case I::Cpu:
            rect(6, 6, 12, 12, 2);
            rect(9, 9, 6, 6, 1);
            for (int n : {8, 12, 16}) {
                line(n, 2, n, 6);
                line(n, 18, n, 22);
                line(2, n, 6, n);
                line(18, n, 22, n);
            }
            break;
        case I::Memory:
            rect(2, 6, 20, 12, 2);
            for (int n : {6, 11, 16}) {
                rect(n, 9, 2, 6, 0, true);
            }
            for (int n : {5, 9, 13, 17}) {
                line(n, 18, n, 21);
            }
            break;
        case I::Heart: {
            SkPath p;
            p.moveTo(12, 21);
            p.cubicTo(2, 14, -1, 7, 5, 3);
            p.cubicTo(8, 1, 11, 3, 12, 6);
            p.cubicTo(13, 3, 16, 1, 19, 3);
            p.cubicTo(25, 7, 22, 14, 12, 21);
            p.close();
            drawPath(p, fill);
        } break;
        case I::Layers:
            path({{12, 3}, {22, 8}, {12, 13}, {2, 8}}, true);
            path({{2, 12}, {12, 17}, {22, 12}});
            path({{2, 16}, {12, 21}, {22, 16}});
            break;
        case I::Rectangle:
            rect(3, 5, 18, 14, 0);
            break;
        case I::Drop: {
            SkPath p;
            p.moveTo(12, 2);
            p.cubicTo(10, 6, 4, 11, 4, 15);
            p.cubicTo(4, 24, 20, 24, 20, 15);
            p.cubicTo(20, 11, 14, 6, 12, 2);
            p.close();
            drawPath(p, stroke);
        } break;
        case I::WifiOff: {
            SkPath p;
            p.moveTo(3, 9);
            p.quadTo(12, 1, 21, 9);
            drawPath(p, stroke);
            SkPath q;
            q.moveTo(6, 13);
            q.quadTo(12, 7, 18, 13);
            drawPath(q, stroke);
            drawCircle(12, 20, 1.4, fill);
            line(3, 3, 21, 21);
        } break;
        case I::Error:
            drawCircle(12, 12, 9, stroke);
            line(8, 8, 16, 16);
            line(16, 8, 8, 16);
            break;
        }

        if (canvas) {
            canvas->restore();
        }

        if (ink) {
            SkMatrix matrix;
            matrix.setScaleTranslate(
                static_cast<SkScalar>(side / 24), static_cast<SkScalar>(side / 24),
                static_cast<SkScalar>(icon.bounds.x + (icon.bounds.width - side) / 2),
                static_cast<SkScalar>(icon.bounds.y + (icon.bounds.height - side) / 2));
            matrix.mapRect(ink, local_ink);
            valid = valid && ink->isFinite();
        }
        return valid;
    }

private:
    SkCanvas *canvas;
    SkRect *ink;
    SkPaint stroke, fill;
    SkRect local_ink;
    bool valid{true};

    void record(const SkRect &raw, const SkPaint &paint)
    {
        if (!ink) {
            return;
        }
        if (!paint.canComputeFastBounds()) {
            valid = false;
            return;
        }
        SkRect storage;
        const auto bounds = paint.computeFastBounds(raw, &storage);
        if (!bounds.isFinite()) {
            valid = false;
            return;
        }
        local_ink.join(bounds);
    }

    void drawLine(float x, float y, float u, float v, const SkPaint &paint)
    {
        record(SkRect::MakeLTRB(std::min(x, u), std::min(y, v), std::max(x, u), std::max(y, v)),
               paint);
        if (canvas) {
            canvas->drawLine(x, y, u, v, paint);
        }
    }

    void drawRoundRect(const SkRect &bounds, float rx, float ry, const SkPaint &paint)
    {
        record(bounds, paint);
        if (canvas) {
            canvas->drawRoundRect(bounds, rx, ry, paint);
        }
    }

    void drawPath(const SkPath &path, const SkPaint &paint)
    {
        record(path.getBounds(), paint);
        if (canvas) {
            canvas->drawPath(path, paint);
        }
    }

    void drawOval(const SkRect &bounds, const SkPaint &paint)
    {
        record(bounds, paint);
        if (canvas) {
            canvas->drawOval(bounds, paint);
        }
    }

    void drawCircle(float x, float y, float radius, const SkPaint &paint)
    {
        record(SkRect::MakeLTRB(x - radius, y - radius, x + radius, y + radius), paint);
        if (canvas) {
            canvas->drawCircle(x, y, radius, paint);
        }
    }

    void line(float x, float y, float u, float v)
    {
        drawLine(x, y, u, v, stroke);
    }

    void rect(float x, float y, float w, float h, float r, bool solid = false)
    {
        drawRoundRect(SkRect::MakeXYWH(x, y, w, h), r, r, solid ? fill : stroke);
    }

    void path(std::initializer_list<SkPoint> points, bool close = false, bool solid = false)
    {
        SkPath p;
        auto it = points.begin();
        if (it == points.end()) {
            return;
        }
        p.moveTo(*it++);
        for (; it != points.end(); ++it) {
            p.lineTo(*it);
        }
        if (close) {
            p.close();
        }
        drawPath(p, solid ? fill : stroke);
    }
};

inline bool WalkIcon(SkCanvas *canvas, const contracts::DrawIcon &icon, SkRect *ink)
{
    VectorIconWalker walker(canvas, ink);
    return walker.Walk(icon);
}

inline void ReplayIcon(SkCanvas *canvas, const contracts::DrawIcon &icon)
{
    (void)WalkIcon(canvas, icon, nullptr);
}

inline bool IconInkBounds(const contracts::DrawIcon &icon, SkRect *ink)
{
    // Singular icon transforms have no reliable geometric stroke footprint.
    // The comparator must choose full damage rather than guess an empty mask.
    if (std::min(icon.bounds.width, icon.bounds.height) <= 0) {
        return false;
    }
    return ink && WalkIcon(nullptr, icon, ink);
}
} // namespace prism::render_skia
