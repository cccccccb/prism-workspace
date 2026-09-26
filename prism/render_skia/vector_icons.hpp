#pragma once
#include "prism/contracts/display_list.hpp"
#include "include/core/SkCanvas.h"
#include "include/core/SkPath.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include <cmath>
#include <algorithm>
#include <initializer_list>

namespace prism::render_skia {
// A shared 24-unit vector resource set. No text glyph or application-specific
// drawing branches enter the Scene; icons replay in raster and GLES alike.
inline void ReplayIcon(SkCanvas* canvas, const contracts::DrawIcon& icon) {
    canvas->save();
    const auto side=std::min(icon.bounds.width,icon.bounds.height);
    canvas->translate(icon.bounds.x+(icon.bounds.width-side)/2,icon.bounds.y+(icon.bounds.height-side)/2);
    canvas->scale(side/24,side/24);
    SkPaint stroke;
    stroke.setAntiAlias(true); stroke.setStyle(SkPaint::kStroke_Style);
    stroke.setStrokeWidth(1.7); stroke.setStrokeCap(SkPaint::kRound_Cap);
    stroke.setStrokeJoin(SkPaint::kRound_Join);
    stroke.setColor(SkColorSetARGB(icon.color.a,icon.color.r,icon.color.g,icon.color.b));
    SkPaint fill=stroke; fill.setStyle(SkPaint::kFill_Style);
    auto line=[&](float x,float y,float u,float v) { canvas->drawLine(x,y,u,v,stroke); };
    auto rect=[&](float x,float y,float w,float h,float r,bool solid=false) {
        canvas->drawRoundRect(SkRect::MakeXYWH(x,y,w,h),r,r,solid?fill:stroke);
    };
    auto path=[&](std::initializer_list<SkPoint> points,bool close=false,bool solid=false) {
        SkPath p; auto it=points.begin(); if(it==points.end())return;
        p.moveTo(*it++); for(;it!=points.end();++it)p.lineTo(*it);
        if(close)p.close();
        canvas->drawPath(p,solid?fill:stroke);
    };
    using I=contracts::VectorIcon;
    switch(icon.icon) {
        case I::Grid:
            for(int y: {4,13})for(int x:{4,13})rect(x,y,7,7,1.7,true);
            break;
        case I::Music:
            line(10,17,10,5);line(10,5,20,3);line(20,3,20,15);line(10,9,20,7);
            canvas->drawOval(SkRect::MakeXYWH(4,15,6,5),fill);canvas->drawOval(SkRect::MakeXYWH(14,13,6,5),fill);break;
        case I::Settings: {
            for(int i=0;i<8;++i) { const float a=i*3.14159265f/4;
                line(12+7*std::cos(a),12+7*std::sin(a),12+10*std::cos(a),12+10*std::sin(a)); }
            canvas->drawCircle(12,12,7,stroke);canvas->drawCircle(12,12,3,stroke);break;
        }
        case I::Folder:
            path({{3,7},{3,5},{9,5},{11,7},{21,7},{21,20},{3,20}},true);line(3,10,21,10);break;
        case I::Terminal:
            rect(2,4,20,16,3);path({{6,9},{9,12},{6,15}});line(12,16,18,16);break;
        case I::Play:path({{7,4},{20,12},{7,20}},true,true);break;
        case I::Pause:rect(5,4,5,16,1,true);rect(14,4,5,16,1,true);break;
        case I::Previous:rect(4,5,2,14,1,true);path({{19,5},{8,12},{19,19}},true,true);break;
        case I::Next:rect(18,5,2,14,1,true);path({{5,5},{16,12},{5,19}},true,true);break;
        case I::Volume:
            path({{3,9},{7,9},{12,5},{12,19},{7,15},{3,15}},true);
            { SkPath p;p.moveTo(16,8);p.cubicTo(20,9,20,15,16,16);canvas->drawPath(p,stroke);
              SkPath q;q.moveTo(19,5);q.cubicTo(25,8,25,16,19,19);canvas->drawPath(q,stroke); }break;
        case I::Wifi:
            {SkPath p;p.moveTo(3,9);p.quadTo(12,1,21,9);canvas->drawPath(p,stroke);
             SkPath q;q.moveTo(6,13);q.quadTo(12,7,18,13);canvas->drawPath(q,stroke);
             SkPath r;r.moveTo(9,16);r.quadTo(12,13,15,16);canvas->drawPath(r,stroke);
             canvas->drawCircle(12,20,1.4,fill);}break;
        case I::Battery:rect(2,7,18,10,2);rect(5,10,12,4,1,true);rect(21,10,2,4,1,true);break;
        case I::Search:canvas->drawCircle(10,10,6,stroke);line(14.5,14.5,21,21);break;
        case I::Sun:
            canvas->drawCircle(12,12,4,stroke);
            for(int i=0;i<8;++i) { const float a=i*3.14159265f/4;
                line(12+7*std::cos(a),12+7*std::sin(a),12+10*std::cos(a),12+10*std::sin(a)); }break;
        case I::Moon:
            {SkPath p;p.moveTo(18,3);p.cubicTo(6,1,1,14,9,20);p.cubicTo(15,24,23,19,22,12);
             p.cubicTo(14,16,10,7,18,3);p.close();canvas->drawPath(p,fill);}break;
        case I::Power:
            {SkPath p;p.moveTo(7,5);p.cubicTo(-1,11,4,22,12,22);p.cubicTo(20,22,25,11,17,5);canvas->drawPath(p,stroke);}
            line(12,2,12,12);break;
        case I::Check:path({{4,12},{10,18},{21,5}});break;
        case I::Chevron:path({{9,5},{16,12},{9,19}});break;
        case I::Refresh:
            {SkPath p;p.moveTo(20,10);p.cubicTo(17,-1,2,2,3,13);p.cubicTo(4,23,18,24,21,15);canvas->drawPath(p,stroke);}
            path({{20,3},{20,10},{13,10}});break;
        case I::Cpu:
            rect(6,6,12,12,2);rect(9,9,6,6,1);
            for(int n:{8,12,16}) {line(n,2,n,6);line(n,18,n,22);line(2,n,6,n);line(18,n,22,n);}break;
        case I::Memory:
            rect(2,6,20,12,2);for(int n:{6,11,16})rect(n,9,2,6,0,true);
            for(int n:{5,9,13,17})line(n,18,n,21);
            break;
        case I::Heart:
            {SkPath p;p.moveTo(12,21);p.cubicTo(2,14,-1,7,5,3);p.cubicTo(8,1,11,3,12,6);
             p.cubicTo(13,3,16,1,19,3);p.cubicTo(25,7,22,14,12,21);p.close();canvas->drawPath(p,fill);}break;
    }
    canvas->restore();
}
}
