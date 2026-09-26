#include "prism/runtime/layout_engine.hpp"
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace prism::runtime {
namespace {
using Size = contracts::LogicalSize;
using Rect = contracts::LogicalRect;
double PadX(const Style& s) { return s.padding_x < 0 ? s.padding : s.padding_x; }
double PadY(const Style& s) { return s.padding_y < 0 ? s.padding : s.padding_y; }
bool Container(Kind kind) { return kind == Kind::Row || kind == Kind::Column || kind == Kind::Box; }
Size Measure(SceneSnapshot& snapshot, contracts::NodeId id, const ShapeText& shaper) {
    auto& node = snapshot.Get(id);
    if (!node.style.visible) return {};
    Size natural{};
    if (node.kind == Kind::Text) {
        node.shaped = shaper(node.text, node.style.font_size);
        natural = {node.shaped.width, node.shaped.height};
    } else if (node.kind == Kind::Image && node.image_ready) natural = node.intrinsic_size;
    else if (node.kind == Kind::Icon || node.kind == Kind::IconButton) natural = {24,24};
    else if (node.kind == Kind::Toggle) natural = {38,22};
    else if (node.kind == Kind::Progress) natural = {80,5};
    else if (node.kind == Kind::Separator) natural = {1,24};
    else {
        std::size_t visible_count=0;
        for (auto child_id : node.children) {
            if (!snapshot.Get(child_id).style.visible) continue;
            ++visible_count;
            auto child = Measure(snapshot, child_id, shaper);
            const auto inset = snapshot.Get(child_id).style.inset * 2;
            child.width += inset; child.height += inset;
            if (node.kind == Kind::Row) {
                natural.width += child.width; natural.height = std::max(natural.height, child.height);
            } else if (node.kind == Kind::Column) {
                natural.height += child.height; natural.width = std::max(natural.width, child.width);
            } else {
                natural.width = std::max(natural.width, child.width);
                natural.height = std::max(natural.height, child.height);
            }
        }
        if (visible_count) {
            const auto gaps = node.style.spacing * (visible_count-1);
            if (node.kind == Kind::Row) natural.width += gaps;
            if (node.kind == Kind::Column) natural.height += gaps;
        }
    }
    natural.width += 2*PadX(node.style);
    natural.height += 2*PadY(node.style);
    if (node.style.width > 0) natural.width = node.style.width;
    if (node.style.height > 0) natural.height = node.style.height;
    node.intrinsic_size = natural;
    return natural;
}
double Offset(std::string_view align, double available, double length) {
    if (align == "center") return std::max(0.0, (available-length)/2);
    if (align == "end" || align == "right") return std::max(0.0, available-length);
    return 0;
}
void Place(SceneSnapshot& snapshot, contracts::NodeId id, Rect bounds) {
    auto& node = snapshot.Get(id);
    if (!node.style.visible) return;
    node.bounds = bounds;
    if (!Container(node.kind)) return;
    std::vector<contracts::NodeId> children;
    for (auto child:node.children) if (snapshot.Get(child).style.visible) children.push_back(child);
    const double px=PadX(node.style), py=PadY(node.style);
    Rect inner{bounds.x+px, bounds.y+py, std::max(0.0,bounds.width-2*px), std::max(0.0,bounds.height-2*py)};
    if (node.kind == Kind::Box) {
        double centered_width=0;
        for(auto child_id:children) {
            const auto& child=snapshot.Get(child_id);
            if(child.style.anchor=="center")centered_width=std::max(centered_width,
                child.style.width>0?child.style.width:child.intrinsic_size.width);
        }
        for (auto child_id : children) {
            const auto& child = snapshot.Get(child_id);
            const auto& style = child.style;
            const double margin = style.inset;
            const double available_w=std::max(0.0,inner.width-2*margin), available_h=std::max(0.0,inner.height-2*margin);
            const double side_limit=centered_width>0 && (style.anchor=="left" || style.anchor=="right")
                ? std::max(0.0,(available_w-centered_width)/2-node.style.spacing) : available_w;
            const bool anchored=style.anchor!="fill";
            const double w = std::min(side_limit, style.width>0 ? style.width : anchored ? child.intrinsic_size.width : available_w);
            const double h = std::min(available_h, style.height>0 ? style.height : child.kind==Kind::Text ? child.intrinsic_size.height : available_h);
            const auto horizontal = style.anchor=="left" ? "start" : style.anchor=="center" ? "center" :
                style.anchor=="right" ? "end" : node.style.align.c_str();
            Place(snapshot, child_id, {inner.x+margin+Offset(horizontal,available_w,w),
                inner.y+margin+Offset(node.style.justify,available_h,h),w,h});
        }
        return;
    }
    const bool row=node.kind==Kind::Row;
    const double main=row?inner.width:inner.height, cross=row?inner.height:inner.width;
    double gap=node.style.spacing;
    double fixed=gap*(children.empty()?0:children.size()-1), total_weight=0;
    std::vector<double> lengths, weights;
    for (auto child_id : children) {
        const auto& child=snapshot.Get(child_id);
        const double explicit_size=row?child.style.width:child.style.height;
        const double weight=child.style.flex>0 ? child.style.flex : explicit_size==0 && Container(child.kind) ? 1 : 0;
        const double length=weight>0 ? 0 : row?child.intrinsic_size.width:child.intrinsic_size.height;
        fixed += length+2*child.style.inset;
        total_weight += weight;
        lengths.push_back(length); weights.push_back(weight);
    }
    const double remaining=std::max(0.0, main-fixed);
    if (total_weight>0) for (std::size_t i=0;i<lengths.size();++i) lengths[i]+=remaining*weights[i]/total_weight;
    double cursor=(row?inner.x:inner.y)+Offset(node.style.justify,main,fixed+(total_weight>0?remaining:0));
    if (total_weight==0 && node.style.justify=="spaceBetween" && lengths.size()>1) gap+=remaining/(lengths.size()-1);
    for (std::size_t i=0;i<children.size();++i) {
        const auto child_id=children[i];
        const auto& child=snapshot.Get(child_id);
        const double margin=child.style.inset;
        const double explicit_cross=row?child.style.height:child.style.width;
        const double available_cross=std::max(0.0,cross-2*margin);
        const double natural_cross=row?child.intrinsic_size.height:child.intrinsic_size.width;
        // An automatic image retains its decoded size in flow containers;
        // explicit dimensions (or a Card fill slot) select a scaling target.
        const double other=std::min(available_cross,explicit_cross>0?explicit_cross:
            node.style.align=="stretch" && child.kind!=Kind::Image?available_cross:natural_cross);
        const double start=(row?inner.y:inner.x)+margin+Offset(node.style.align,available_cross,other);
        Place(snapshot,child_id,row?Rect{cursor+margin,start,lengths[i],other}:Rect{start,cursor+margin,other,lengths[i]});
        cursor += lengths[i]+2*margin+gap;
    }
}
} // namespace
void LayoutEngine::Compute(SceneSnapshot& snapshot, contracts::LogicalSize viewport, const ShapeText& shaper) {
    if (!snapshot.root || !shaper) throw std::invalid_argument("Layout needs a root and text shaper");
    for (auto& node:snapshot.nodes) node.bounds={};
    Measure(snapshot,snapshot.root,shaper);
    const auto inset=snapshot.Get(snapshot.root).style.inset;
    Place(snapshot,snapshot.root,{inset,inset,std::max(0.0,viewport.width-2*inset),std::max(0.0,viewport.height-2*inset)});
}
} // namespace prism::runtime
