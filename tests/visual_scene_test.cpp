#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"
#include <cassert>
#include <cmath>
#include <stdexcept>
#include <variant>

using namespace prism;
namespace {
runtime::ShapedText Shape(std::string_view text, double size) {
    runtime::ShapedText result;
    result.width=text.size()*7;
    result.height=size+2;
    if(!text.empty())result.glyphs.push_back({42,{0,size}});
    return result;
}
}
int main() {
    runtime::Scene bar(runtime::ParseBlueprint(R"(
        Card(justify:"center") {
            HStack(width:110,height:24,anchor:"left") { Icon("grid",width:24,height:24) }
            Text("12:00",width:120,anchor:"center",font:14)
            HStack(width:180,height:24,anchor:"right") { Icon("wifi",width:24,height:24) }
        })"),Shape,{7});
    assert(bar.SetViewport({1024,32}));assert(bar.Build({1}));
    const auto clock=bar.Bounds({3,1});
    assert(clock.x==452 && clock.width==120 && clock.y==8);
    assert(bar.SetViewport({600,32}));assert(bar.Build({1}));
    assert(bar.Bounds({3,1}).x==240); // side widths never shift the global center
    assert(bar.SetViewport({250,32}));assert(bar.Build({1}));
    assert(bar.Bounds({1,1}).x+bar.Bounds({1,1}).width<=bar.Bounds({3,1}).x);
    assert(bar.Bounds({4,1}).x>=bar.Bounds({3,1}).x+bar.Bounds({3,1}).width);

    runtime::Scene flex(runtime::ParseBlueprint(R"(
        HStack(spacing:10,align:"center") {
            Card(flex:1,height:20) Card(flex:2,height:30)
        })"),Shape);
    assert(flex.SetViewport({310,50}));assert(flex.Build({1}));
    assert(flex.Bounds({1,1}).width==100 && flex.Bounds({2,1}).width==200);
    assert(flex.Bounds({1,1}).y==15 && flex.Bounds({2,1}).y==10);

    runtime::Scene controls(runtime::ParseBlueprint(R"(
        Card { HStack(width:140,height:40,anchor:"center",overflow:"clip") {
            IconButton($icon,"play",width:40,height:40,cornerRadius:12,clip:true,background:"@panelTint")
            Progress(value:$progress,width:60,height:5,foreground:"@accent")
            Toggle(checked:$enabled,action:"toggle",width:38,height:22)
        } })"),Shape);
    assert(controls.SetBinding("icon",std::string("play")));
    assert(!controls.SetBinding("icon",std::string("unknown")));
    assert(controls.SetBinding("progress",.5));assert(!controls.SetBinding("progress",1.1));
    assert(controls.SetBinding("enabled",true));assert(!controls.SetBinding("enabled",std::string("true")));
    assert(controls.SetViewport({200,50}));auto list=controls.Build({1});assert(list);
    assert(controls.ActionAt({50,20})=="play");
    assert(!controls.ActionAt({30,0})); // rounded transparent corner is not interactive
    assert(controls.SetPointer({50,20}));assert(controls.Build({1}));
    assert(controls.FocusNext());assert(controls.FocusedAction()=="play");
    assert(controls.FocusNext());assert(controls.FocusedAction()=="toggle");
    bool icon=false,rounded_clip=false;
    for(const auto& command:list->commands) {
        icon |= std::holds_alternative<contracts::DrawIcon>(command);
        rounded_clip |= std::holds_alternative<contracts::PushClipRoundedRect>(command);
    }
    assert(icon && rounded_clip);

    runtime::Scene glass(runtime::ParseBlueprint(R"(
        Card { Card(width:100,height:30,inset:10,background:"@panelTint",cornerRadius:12,backdropBlur:16) }
    )"),Shape);
    assert(glass.SetViewport({140,50}));assert(glass.Build({1}));
    const auto effects=glass.SurfaceEffects();const auto input=glass.InputRegions();
    assert(effects.size()==1 && effects[0].bounds.x==10 && effects[0].corner_radius==12 && effects[0].blur_radius==16);
    assert(input.size()==1 && input[0].bounds.x==10 && input[0].bounds.width==100);
    runtime::Scene clipped(runtime::ParseBlueprint(R"(
        Card(clip:true) { HStack(width:60,height:20,overflow:"clip") {
            IconButton("play","play",width:120,height:20,background:#FFFFFFFF)
        } }
    )"),Shape);
    assert(clipped.SetViewport({100,30}));assert(clipped.Build({1}));
    assert(clipped.Bounds({2,1}).width==120); // intrinsic contents retain size, clip handles overflow
    const auto regions=clipped.InputRegions();
    assert(regions.size()==1 && regions[0].bounds.width==60);
    assert(!clipped.ActionAt({70,10}));
    runtime::Scene round_parent(runtime::ParseBlueprint(R"(
        Card(clip:true,cornerRadius:20) {
            IconButton("play","play",width:40,height:40,background:#FFFFFFFF)
        }
    )"),Shape);
    assert(round_parent.SetViewport({80,40}));assert(round_parent.Build({1}));
    const auto& rounded_regions=round_parent.InputRegions();
    assert(rounded_regions.size()>1 && rounded_regions.front().corner_radius==0);
    assert(rounded_regions.front().bounds.y==0 && rounded_regions.front().bounds.x>=15);
    const auto region_contains=[&](contracts::LogicalPoint point) {
        for(const auto& region:rounded_regions)if(point.x>=region.bounds.x && point.y>=region.bounds.y &&
            point.x<region.bounds.x+region.bounds.width && point.y<region.bounds.y+region.bounds.height)return true;
        return false;
    };
    assert(!region_contains({1,1}) && !round_parent.ActionAt({1,1}));
    assert(region_contains({30,10}) && round_parent.ActionAt({30,10})=="play");
    // The effect request must describe visible material, rather than the
    // oversized flow item that layout intentionally leaves to its clip.
    runtime::Scene clipped_backdrop(runtime::ParseBlueprint(R"(
        Card(clip:true) { HStack(width:80,height:30,overflow:"clip") {
            Card(width:120,height:30,backdropBlur:12)
        } }
    )"),Shape);
    assert(clipped_backdrop.SetViewport({100,40}));assert(clipped_backdrop.Build({1}));
    assert(clipped_backdrop.Bounds({2,1}).width==120);
    const auto clipped_effects=clipped_backdrop.SurfaceEffects();
    assert(clipped_effects.size()==1 && clipped_effects[0].bounds.width==80 &&
        clipped_effects[0].bounds.height==30 && clipped_effects[0].corner_radius==0);
    runtime::Scene rounded_backdrop(runtime::ParseBlueprint(
        "Card(clip:true,cornerRadius:20) { Card(backdropBlur:12) }"),Shape);
    assert(rounded_backdrop.SetViewport({80,40}));assert(rounded_backdrop.Build({1}));
    assert(rounded_backdrop.SurfaceEffects().front().corner_radius==20);
    runtime::Scene unsupported_backdrop(runtime::ParseBlueprint(
        "Card(clip:true,cornerRadius:20) { Card(width:40,height:40,backdropBlur:12) }"),Shape);
    assert(unsupported_backdrop.SetViewport({80,40}));assert(unsupported_backdrop.Build({1}));
    bool unsupported=false;
    try{(void)unsupported_backdrop.SurfaceEffects();}catch(const std::runtime_error& error) {
        unsupported=std::string_view(error.what()).find("Unsupported backdrop clipping")!=std::string_view::npos;
    }
    assert(unsupported); // an asymmetric curved corner cannot be sent as a uniform-radius rect
    runtime::Scene oversized_backdrop(runtime::ParseBlueprint("Card(backdropBlur:12)"),Shape);
    assert(oversized_backdrop.SetViewport({9000,40}));assert(oversized_backdrop.Build({1}));
    bool oversized=false;
    try{(void)oversized_backdrop.SurfaceEffects();}catch(const std::runtime_error& error) {
        oversized=std::string_view(error.what()).find("Unsupported backdrop bounds")!=std::string_view::npos;
    }
    assert(oversized); // never transmit a request that violates the Wayland v1 extent limit
    for(auto source: {"Card(background:\"@missing\")", "Card(background:\"@font_body\")", "Card(backdropBlur:49)"}) {
        bool rejected=false;try{(void)runtime::ParseBlueprint(source);}catch(const std::runtime_error&){rejected=true;}
        assert(rejected);
    }
    bool too_many=false;
    try {
        (void)runtime::ParseBlueprint("Card { Card(backdropBlur:1) Card(backdropBlur:1) Card(backdropBlur:1) "
            "Card(backdropBlur:1) Card(backdropBlur:1) Card(backdropBlur:1) Card(backdropBlur:1) "
            "Card(backdropBlur:1) Card(backdropBlur:$dynamic) }");
    } catch(const std::runtime_error&) {too_many=true;}
    assert(too_many);
    runtime::Scene intrinsic(runtime::ParseBlueprint("HStack(align:\"center\") { Text(\"WWW\",font:14) Text(\"i\",font:14) }"),Shape);
    assert(intrinsic.SetViewport({200,30}));assert(intrinsic.Build({1}));
    assert(intrinsic.Bounds({1,1}).width==21 && intrinsic.Bounds({2,1}).width==7);
}
