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
contracts::ThemeSnapshot Theme() {
    contracts::ThemeSnapshot result;
    result.generation=1;result.id="glass";result.name="Glass";
    result.numbers={{"font_body",14}};
    result.colors={{"panelTint",{240,245,250,180}},{"accent",{50,110,220,255}}};
    contracts::ThemeMaterial window;
    window.name="window";window.tint={230,240,250,180};window.radius=12;
    window.backdrop_blur=12;window.input_shape=contracts::ThemeInputShape::Bounds;
    result.materials.push_back(window);
    result.controls.hover={255,255,255,28};result.controls.focus={110,170,250,230};
    result.controls.focus_width=2;result.controls.toggle_knob={255,255,255,255};
    result.controls.toggle_inset=2;result.controls.toggle_knob_radius=11;
    result.controls.toggle_track_radius=11;result.controls.inner_shadow_y=1;
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
        } })"),Shape,{},Theme());
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
    )"),Shape,{},Theme());
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
        bool rejected=false;
        try{runtime::Scene invalid(runtime::ParseBlueprint(source),Shape,{},Theme());}
        catch(const std::exception&){rejected=true;}
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

    // Live transitions reuse nodes, business values and ready image resources.
    runtime::Scene live(runtime::ParseBlueprint(R"(
        Card(material:"window",clip:true) { VStack(spacing:0) {
            Button($label,"go",height:30,font:"@font_body",foreground:"@accent")
            Toggle(checked:$enabled,action:"toggle",width:38,height:22,foreground:"@accent")
            Progress(value:$progress,width:60,height:5,foreground:"@accent")
            Image("fixture",width:20,height:20)
        } }
    )",[](std::string_view){return contracts::ResourceId{99};}),Shape,{7},Theme());
    assert(live.SetViewport({100,100}));assert(live.SetBinding("label",std::string("Playing")));
    assert(live.SetBinding("enabled",true));assert(live.SetBinding("progress",.7));
    assert(live.ImageReady({99},{20,20}));assert(live.Build({1}));
    const auto root_id=live.RootId();
    const auto image_bounds=live.Bounds({6,1});
    assert(live.SurfaceEffects().size()==1 && live.InputRegions().size()==1);
    auto clear=Theme();clear.generation=2;clear.id="transparent";clear.name="Transparent";
    clear.materials[0].tint={0,0,0,0};clear.materials[0].backdrop_blur=0;
    clear.colors[1].value={230,30,50,255};
    std::string diagnostic;
    assert(live.ApplyTheme(clear,&diagnostic));assert(diagnostic.empty());
    auto clear_list=live.Build({1});assert(clear_list && live.ThemeGeneration()==2 && live.RootId()==root_id);
    assert(live.SurfaceEffects().empty());
    assert(live.InputRegions().size()==1 && live.InputRegions().front().bounds.width==100);
    bool retained_image=false, recolored_label=false, retained_progress=false;
    for(const auto& command:clear_list->commands) {
        if(const auto* image=std::get_if<contracts::DrawImage>(&command))
            retained_image=image->image.value==99 && image->destination.x==image_bounds.x;
        if(const auto* run=std::get_if<contracts::DrawGlyphRun>(&command))
            recolored_label=run->color==clear.colors[1].value && !run->glyphs.empty();
        if(const auto* rect=std::get_if<contracts::FillRoundedRect>(&command))
            retained_progress|=std::abs(rect->bounds.width-42)<1e-7 && rect->color==clear.colors[1].value;
    }
    assert(retained_image && recolored_label && retained_progress);
    assert(live.ActionAt({50,15})=="go");assert(live.SetBinding("label",std::string("Paused")));
    auto square=clear;square.id="square";square.name="Square";square.generation=3;
    square.materials[0].radius=0;square.controls.toggle_knob_radius=0;square.controls.toggle_track_radius=0;
    assert(live.ApplyTheme(square));auto square_list=live.Build({1});assert(square_list);
    assert(live.InputRegions().front().corner_radius==0);
    bool square_knob=false;
    for(const auto& command:square_list->commands) if(const auto* rect=std::get_if<contracts::FillRoundedRect>(&command))
        square_knob|=rect->color==square.controls.toggle_knob && rect->radius==0;
    assert(square_knob && live.RootId()==root_id && live.ThemeGeneration()==3);
    auto missing=square;missing.generation=4;missing.colors.pop_back();
    assert(!live.ApplyTheme(missing,&diagnostic) && !diagnostic.empty());
    assert(live.ThemeGeneration()==3 && live.PendingDirty()==runtime::Dirty::None);
    assert(live.ApplyTheme(square,&diagnostic) && diagnostic.empty());
    assert(!live.Build({1}) && live.ActionAt({50,15})=="go");
    assert(live.SetBinding("enabled",false));assert(live.Build({1}));

    // An unsupported rounded effect intersection rejects the entire candidate.
    auto flat=Theme();flat.materials[0].radius=0;flat.materials[0].backdrop_blur=0;
    runtime::Scene rollback(runtime::ParseBlueprint(
        "Card(material:\"window\",clip:true) { Card(width:40,height:40,backdropBlur:12) }"),Shape,{},flat);
    assert(rollback.SetViewport({80,40}));assert(rollback.Build({1}));
    const auto old_effects=rollback.SurfaceEffects();
    auto curved=flat;curved.generation=2;curved.materials[0].radius=20;
    assert(!rollback.ApplyTheme(curved,&diagnostic));
    assert(diagnostic.find("Unsupported backdrop clipping")!=std::string::npos);
    assert(rollback.ThemeGeneration()==1 && rollback.PendingDirty()==runtime::Dirty::None);
    assert(rollback.SurfaceEffects().front().bounds.width==old_effects.front().bounds.width);
    assert(rollback.InputRegions().front().corner_radius==0);
}
