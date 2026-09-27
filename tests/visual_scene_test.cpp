#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"
#include <cassert>
#include <cmath>
#include <stdexcept>
#include <variant>

using namespace prism;
namespace {
// These visual tests model a successful protocol metadata commit after each
// build. Scene::Build alone is not an acknowledgement from the surface.
std::optional<contracts::DisplayList> BuildAndCommit(runtime::Scene& scene) {
    auto list=scene.Build({1});
    scene.AcknowledgeComposite();
    return list;
}
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
    assert(bar.SetViewport({1024,32}));assert(BuildAndCommit(bar));
    const auto clock=bar.Bounds({3,1});
    assert(clock.x==452 && clock.width==120 && clock.y==8);
    assert(bar.SetViewport({600,32}));assert(BuildAndCommit(bar));
    assert(bar.Bounds({3,1}).x==240); // side widths never shift the global center
    assert(bar.SetViewport({250,32}));assert(BuildAndCommit(bar));
    assert(bar.Bounds({1,1}).x+bar.Bounds({1,1}).width<=bar.Bounds({3,1}).x);
    assert(bar.Bounds({4,1}).x>=bar.Bounds({3,1}).x+bar.Bounds({3,1}).width);

    // Explicit anchors select the parent's edges, regardless of its default
    // alignment. In particular a centered Box must not center its left item.
    runtime::Scene anchored(runtime::ParseBlueprint(R"(
        Card(align:"center",justify:"center",padding:10) {
            Card(anchor:"left",width:30,height:10)
            Card(anchor:"center",width:20,height:10)
            Card(anchor:"right",width:25,height:10)
        }
    )"),Shape);
    assert(anchored.SetViewport({180,60}));assert(BuildAndCommit(anchored));
    assert(anchored.Bounds({1,1}).x==10 && anchored.Bounds({1,1}).y==25);
    assert(anchored.Bounds({2,1}).x==80 && anchored.Bounds({2,1}).y==25);
    assert(anchored.Bounds({3,1}).x==145 && anchored.Bounds({3,1}).y==25);
    assert(anchored.SetViewport({120,60}));assert(BuildAndCommit(anchored));
    assert(anchored.Bounds({1,1}).x==10 && anchored.Bounds({2,1}).x==50);
    assert(anchored.Bounds({3,1}).x==85);

    runtime::Scene flex(runtime::ParseBlueprint(R"(
        HStack(spacing:10,align:"center") {
            Card(flex:1,height:20) Card(flex:2,height:30)
        })"),Shape);
    assert(flex.SetViewport({310,50}));assert(BuildAndCommit(flex));
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
    assert(controls.SetViewport({200,50}));auto list=BuildAndCommit(controls);assert(list);
    assert(controls.ActionAt({50,20})=="play");
    assert(!controls.ActionAt({30,0})); // rounded transparent corner is not interactive
    assert(controls.SetPointer({50,20}));assert(BuildAndCommit(controls));
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
    assert(glass.SetViewport({140,50}));assert(BuildAndCommit(glass));
    const auto effects=glass.SurfaceEffects();const auto input=glass.InputRegions();
    assert(effects.size()==1 && effects[0].bounds.x==10 && effects[0].corner_radius==12 && effects[0].blur_radius==16);
    assert(input.size()==1 && input[0].bounds.x==10 && input[0].bounds.width==100);
    runtime::Scene clipped(runtime::ParseBlueprint(R"(
        Card(clip:true) { HStack(width:60,height:20,overflow:"clip") {
            IconButton("play","play",width:120,height:20,background:#FFFFFFFF)
        } }
    )"),Shape);
    assert(clipped.SetViewport({100,30}));assert(BuildAndCommit(clipped));
    assert(clipped.Bounds({2,1}).width==120); // intrinsic contents retain size, clip handles overflow
    const auto regions=clipped.InputRegions();
    assert(regions.size()==1 && regions[0].bounds.width==60);
    assert(!clipped.ActionAt({70,10}));
    runtime::Scene round_parent(runtime::ParseBlueprint(R"(
        Card(clip:true,cornerRadius:20) {
            IconButton("play","play",width:40,height:40,background:#FFFFFFFF)
        }
    )"),Shape);
    assert(round_parent.SetViewport({80,40}));assert(BuildAndCommit(round_parent));
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
    assert(clipped_backdrop.SetViewport({100,40}));assert(BuildAndCommit(clipped_backdrop));
    assert(clipped_backdrop.Bounds({2,1}).width==120);
    const auto clipped_effects=clipped_backdrop.SurfaceEffects();
    assert(clipped_effects.size()==1 && clipped_effects[0].bounds.width==80 &&
        clipped_effects[0].bounds.height==30 && clipped_effects[0].corner_radius==0);
    runtime::Scene rounded_backdrop(runtime::ParseBlueprint(
        "Card(clip:true,cornerRadius:20) { Card(backdropBlur:12) }"),Shape);
    assert(rounded_backdrop.SetViewport({80,40}));assert(BuildAndCommit(rounded_backdrop));
    assert(rounded_backdrop.SurfaceEffects().front().corner_radius==20);
    runtime::Scene unsupported_backdrop(runtime::ParseBlueprint(
        "Card(clip:true,cornerRadius:20) { Card(width:40,height:40,backdropBlur:12) }"),Shape);
    assert(unsupported_backdrop.SetViewport({80,40}));assert(BuildAndCommit(unsupported_backdrop));
    bool unsupported=false;
    try{(void)unsupported_backdrop.SurfaceEffects();}catch(const std::runtime_error& error) {
        unsupported=std::string_view(error.what()).find("Unsupported backdrop clipping")!=std::string_view::npos;
    }
    assert(unsupported); // an asymmetric curved corner cannot be sent as a uniform-radius rect
    runtime::Scene oversized_backdrop(runtime::ParseBlueprint("Card(backdropBlur:12)"),Shape);
    assert(oversized_backdrop.SetViewport({9000,40}));assert(BuildAndCommit(oversized_backdrop));
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
    assert(intrinsic.SetViewport({200,30}));assert(BuildAndCommit(intrinsic));
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
    assert(live.ImageReady({99},{20,20}));assert(BuildAndCommit(live));
    const auto root_id=live.RootId();
    const auto image_bounds=live.Bounds({6,1});
    assert(live.SurfaceEffects().size()==1 && live.InputRegions().size()==1);
    auto clear=Theme();clear.generation=2;clear.id="transparent";clear.name="Transparent";
    clear.materials[0].tint={0,0,0,0};clear.materials[0].backdrop_blur=0;
    clear.colors[1].value={230,30,50,255};
    std::string diagnostic;
    assert(live.ApplyTheme(clear,&diagnostic));assert(diagnostic.empty());
    auto clear_list=BuildAndCommit(live);assert(clear_list && live.ThemeGeneration()==2 && live.RootId()==root_id);
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
    assert(live.ApplyTheme(square));auto square_list=BuildAndCommit(live);assert(square_list);
    assert(live.InputRegions().front().corner_radius==0);
    bool square_knob=false;
    for(const auto& command:square_list->commands) if(const auto* rect=std::get_if<contracts::FillRoundedRect>(&command))
        square_knob|=rect->color==square.controls.toggle_knob && rect->radius==0;
    assert(square_knob && live.RootId()==root_id && live.ThemeGeneration()==3);
    auto missing=square;missing.generation=4;missing.colors.pop_back();
    assert(!live.ApplyTheme(missing,&diagnostic) && !diagnostic.empty());
    assert(live.ThemeGeneration()==3 && live.PendingDirty()==runtime::Dirty::None);
    assert(live.ApplyTheme(square,&diagnostic) && diagnostic.empty());
    assert(!BuildAndCommit(live) && live.ActionAt({50,15})=="go");
    assert(live.SetBinding("enabled",false));assert(BuildAndCommit(live));

    // An unsupported rounded effect intersection rejects the entire candidate.
    auto flat=Theme();flat.materials[0].radius=0;flat.materials[0].backdrop_blur=0;
    runtime::Scene rollback(runtime::ParseBlueprint(
        "Card(material:\"window\",clip:true) { Card(width:40,height:40,backdropBlur:12) }"),Shape,{},flat);
    assert(rollback.SetViewport({80,40}));assert(BuildAndCommit(rollback));
    const auto old_effects=rollback.SurfaceEffects();
    auto curved=flat;curved.generation=2;curved.materials[0].radius=20;
    assert(!rollback.ApplyTheme(curved,&diagnostic));
    assert(diagnostic.find("Unsupported backdrop clipping")!=std::string::npos);
    assert(rollback.ThemeGeneration()==1 && rollback.PendingDirty()==runtime::Dirty::None);
    assert(rollback.SurfaceEffects().front().bounds.width==old_effects.front().bounds.width);
    assert(rollback.InputRegions().front().corner_radius==0);

    // A concealed subtree keeps its state but contributes no flow gap,
    // pixels, effect requests or input. Reshowing shapes its latest text.
    std::string last_shaped;
    runtime::Scene visibility(runtime::ParseBlueprint(R"(
        HStack(spacing:10,align:"center",visible:$root_visible) {
            IconButton("play","left",width:20,height:20,background:#102030FF)
            Card(width:40,height:20,visible:$details_visible,clip:true,
                 background:#7B0A63FF,backdropBlur:12,inputShape:"bounds") {
                Button($caption,"details",font:10,foreground:"@accent")
                Image("fixture",width:4,height:4)
            }
            IconButton("next","right",width:20,height:20,background:#203040FF)
        }
    )",[](std::string_view){return contracts::ResourceId{91};}),
        [&](std::string_view text,double size){last_shaped=text;return Shape(text,size);},{7},Theme());
    assert(visibility.SetViewport({100,20}));
    assert(visibility.AcceptsBinding("root_visible",true));
    assert(visibility.AcceptsBinding("details_visible",true));
    assert(visibility.SetBinding("caption",std::string("Old")));
    assert(visibility.ImageReady({91},{4,4}));assert(BuildAndCommit(visibility));
    const auto retained_root=visibility.RootId();
    assert(visibility.Bounds({2,1}).x==30 && visibility.Bounds({6,1}).x==80);
    assert(visibility.IsVisible({4,1}) && visibility.ActionAt({50,10})=="details");
    assert(visibility.SurfaceEffects().size()==1);
    assert(visibility.FocusNext() && visibility.FocusedAction()=="left");
    assert(visibility.FocusNext() && visibility.FocusedAction()=="details");
    assert(visibility.SetBinding("details_visible",false));
    assert(!visibility.SetBinding("details_visible",std::string("false")));
    assert(!visibility.IsVisible({4,1}) && !visibility.FocusedAction());
    assert(!visibility.ActionAt({50,10})); // hidden immediately, before layout flush
    assert(visibility.SetBinding("caption",std::string("Latest")));
    last_shaped.clear();auto concealed=BuildAndCommit(visibility);assert(concealed);
    assert(last_shaped.empty());
    assert(visibility.Bounds({2,1}).width==0 && visibility.Bounds({3,1}).width==0);
    assert(visibility.Bounds({6,1}).x==30 && visibility.SurfaceEffects().empty());
    assert(visibility.InputRegions().size()==2);
    for(const auto& region:visibility.InputRegions())
        assert(region.bounds.x+region.bounds.width<=50);
    const contracts::Color hidden_tint{123,10,99,255};
    for(const auto& command:concealed->commands) {
        assert(!std::holds_alternative<contracts::DrawGlyphRun>(command));
        assert(!std::holds_alternative<contracts::DrawImage>(command));
        if(const auto* fill=std::get_if<contracts::FillRect>(&command))
            assert(fill->color!=hidden_tint);
    }
    assert(visibility.FocusNext() && visibility.FocusedAction()=="left");
    assert(visibility.FocusNext() && visibility.FocusedAction()=="right");
    assert(BuildAndCommit(visibility)); // Finish the visible focus change first.
    auto while_hidden=Theme();while_hidden.generation=2;while_hidden.id="square";
    const auto hidden_pixels=visibility.PixelsRevision();
    const auto hidden_builds=visibility.GetRenderStats();
    assert(visibility.ApplyTheme(while_hidden));assert(!BuildAndCommit(visibility));
    assert(visibility.ThemeGeneration()==2 && visibility.PixelsRevision()==hidden_pixels);
    assert(visibility.GetRenderStats().builds==hidden_builds.builds);
    assert(!visibility.IsVisible({4,1}) && last_shaped.empty());
    assert(visibility.SetBinding("details_visible",true));auto revealed=BuildAndCommit(visibility);assert(revealed);
    assert(last_shaped=="Latest" && visibility.RootId()==retained_root && visibility.IsVisible({4,1}));
    assert(visibility.Bounds({6,1}).x==80 && visibility.SurfaceEffects().size()==1);
    bool restored_image=false;
    for(const auto& command:revealed->commands) if(const auto* image=std::get_if<contracts::DrawImage>(&command))
        restored_image=image->image.value==91;
    assert(restored_image && visibility.ActionAt({50,10})=="details");
    assert(visibility.SetBinding("root_visible",false));auto hidden_root=BuildAndCommit(visibility);assert(hidden_root);
    assert(hidden_root->commands.empty() && visibility.InputRegions().empty() && visibility.SurfaceEffects().empty());
    assert(!visibility.IsVisible({1,1}) && !visibility.FocusNext() && !visibility.FocusedAction());
    assert(visibility.SetBinding("root_visible",true));assert(BuildAndCommit(visibility));
    assert(visibility.ActionAt({50,10})=="details" && visibility.IsVisible({4,1}));

    // Hidden business updates retain their newest values without scheduling a
    // frame. Effective visibility includes ancestors, including when a child's
    // own visible binding changes back to true while its panel is still hidden.
    std::vector<std::string> deferred_shapes;
    runtime::Scene deferred(runtime::ParseBlueprint(R"(
        VStack(spacing:0) {
            Card(height:50,visible:$panel_visible,clip:true,background:#203040FF) {
                VStack(spacing:0) {
                    Button($caption,"activate",height:20,font:12,foreground:"@accent")
                    Progress(value:$progress,width:40,height:4,foreground:"@accent")
                    Icon("heart",width:12,height:12,visible:$marker_visible,foreground:"@accent")
                    Text($shared,height:12,font:10)
                }
            }
            Text($shared,height:20,font:10)
        }
    )"),[&](std::string_view text,double size) {
        deferred_shapes.emplace_back(text); return Shape(text,size);
    },{7},Theme());
    assert(deferred.SetViewport({100,110}));
    assert(deferred.SetBinding("caption",std::string("Old")));
    assert(deferred.SetBinding("shared",std::string("Shared old")));
    assert(deferred.SetBinding("progress",.25));assert(BuildAndCommit(deferred));
    const auto first_work=deferred.GetRenderStats();
    assert(first_work.build_calls==1 && first_work.builds==1 && first_work.layouts==1);
    // Repeating accepted values does not create scene work. Calling Build on
    // that clean scene is observable as a call, rather than a new frame/layout.
    assert(!deferred.SetBinding("caption",std::string("Old")));
    assert(!deferred.SetBinding("progress",.25));
    assert(deferred.GetRenderStats()==first_work && !BuildAndCommit(deferred));
    const auto clean_work=deferred.GetRenderStats();
    assert(clean_work.build_calls==first_work.build_calls+1);
    assert(clean_work.builds==first_work.builds && clean_work.layouts==first_work.layouts);
    assert(deferred.ActionAt({10,10})=="activate");
    assert(deferred.FocusNext() && deferred.FocusedAction()=="activate");
    assert(deferred.SetPointer({10,10}));assert(BuildAndCommit(deferred));
    const auto hover_work=deferred.GetRenderStats();
    assert(hover_work.builds==clean_work.builds+1 && hover_work.layouts==clean_work.layouts);
    assert(deferred.SetBinding("panel_visible",false));assert(BuildAndCommit(deferred));
    assert(!deferred.FocusedAction() && !deferred.ActionAt({10,10}));
    assert(deferred.InputRegions().empty()); // Populate the concealed input cache.
    const auto concealed_generation=deferred.Generation();
    const auto concealed_work=deferred.GetRenderStats();
    deferred_shapes.clear();
    const contracts::Color latest_text{12,34,56,255},latest_progress{23,45,67,255};
    assert(deferred.SetBinding("caption",std::string("Latest hidden")));
    assert(deferred.SetBinding("progress",.75));
    assert(deferred.SetProperty({1,1},runtime::DslProperty::Padding,5.0));
    assert(deferred.SetProperty({1,1},runtime::DslProperty::Height,90.0));
    assert(deferred.SetBackground({1,1},{0,0,0,0}));
    assert(deferred.SetProperty({3,1},runtime::DslProperty::Height,30.0));
    assert(deferred.SetProperty({4,1},runtime::DslProperty::Font,18.0));
    assert(deferred.SetProperty({4,1},runtime::DslProperty::Foreground,latest_text));
    assert(deferred.SetProperty({5,1},runtime::DslProperty::Width,60.0));
    assert(deferred.SetProperty({5,1},runtime::DslProperty::Foreground,latest_progress));
    assert(deferred.SetBinding("marker_visible",false));
    assert(deferred.SetBinding("marker_visible",true));
    assert(!deferred.IsVisible({6,1}));
    assert(deferred.GetRenderStats()==concealed_work);
    assert(deferred.PendingDirty()==runtime::Dirty::None && !BuildAndCommit(deferred));
    const auto hidden_work=deferred.GetRenderStats();
    assert(hidden_work.build_calls==concealed_work.build_calls+1);
    assert(hidden_work.builds==concealed_work.builds && hidden_work.layouts==concealed_work.layouts);
    assert(deferred.Generation()==concealed_generation && deferred_shapes.empty());
    assert(deferred.InputRegions().empty());
    assert(!deferred.SetPointer({10,10})); // Concealing already cleared old hover.

    // A shared binding still invalidates its visible target even though another
    // target is concealed. Only the visible text is shaped in this frame.
    assert(deferred.SetBinding("shared",std::string("Shared new")));
    assert(runtime::Has(deferred.PendingDirty(),runtime::Dirty::Layout));
    assert(BuildAndCommit(deferred));
    assert(deferred_shapes.size()==1 && deferred_shapes.front()=="Shared new");
    auto deferred_theme=Theme();deferred_theme.generation=2;deferred_theme.colors[1].value={90,80,70,255};
    const auto before_theme_work=deferred.GetRenderStats();
    const auto before_theme_pixels=deferred.PixelsRevision();
    const auto before_theme_generation=deferred.Generation();
    assert(deferred.ApplyTheme(deferred_theme));
    assert(deferred.GetRenderStats()==before_theme_work); // Candidate validation is a separate scene.
    // Text/progress retain explicit overrides; the remaining accent reference
    // belongs to the hidden icon. Install its style/version without drawing it.
    assert(deferred.ThemeGeneration()==2 && deferred.PixelsRevision()==before_theme_pixels);
    assert(deferred.PendingDirty()==runtime::Dirty::None && !BuildAndCommit(deferred));
    assert(deferred.Generation()==before_theme_generation);
    assert(deferred.GetRenderStats().builds==before_theme_work.builds);
    assert(deferred.GetRenderStats().layouts==before_theme_work.layouts);
    deferred_shapes.clear();
    assert(deferred.SetBinding("panel_visible",true));
    assert(runtime::Has(deferred.PendingDirty(),runtime::Dirty::Layout));
    const auto before_reveal_work=deferred.GetRenderStats();
    const auto latest_list=BuildAndCommit(deferred);assert(latest_list);
    const auto revealed_work=deferred.GetRenderStats();
    assert(revealed_work.builds==before_reveal_work.builds+1 && revealed_work.layouts==before_reveal_work.layouts+1);
    assert(deferred_shapes.size()==3 && deferred_shapes.front()=="Latest hidden");
    assert(deferred.IsVisible({6,1}) && deferred.Bounds({3,1}).x==5 && deferred.Bounds({3,1}).height==30);
    assert(deferred.Bounds({5,1}).width==60 && deferred.ActionAt({10,20})=="activate");
    assert(deferred.InputRegions().size()==1 && deferred.InputRegions().front().bounds.width==90);
    assert(!deferred.FocusedAction());
    bool latest_glyph=false,latest_bar=false,marker_restored=false;
    for (const auto& command:latest_list->commands) {
        if (const auto* glyph=std::get_if<contracts::DrawGlyphRun>(&command))
            latest_glyph|=glyph->color==latest_text && glyph->font_size==18;
        if (const auto* bar=std::get_if<contracts::FillRoundedRect>(&command)) {
            latest_bar|=bar->color==latest_progress && bar->bounds.width==45;
            assert(bar->color!=deferred_theme.controls.hover);
        }
        if (const auto* border=std::get_if<contracts::StrokeRoundedRect>(&command))
            assert(border->color!=deferred_theme.controls.focus);
        if (const auto* icon=std::get_if<contracts::DrawIcon>(&command))
            marker_restored|=icon->icon==contracts::VectorIcon::Heart && icon->color==deferred_theme.colors[1].value;
    }
    assert(latest_glyph && latest_bar && marker_restored);
    assert(deferred.PendingDirty()==runtime::Dirty::None && !BuildAndCommit(deferred));

    runtime::Scene hidden_flex(runtime::ParseBlueprint(R"(
        HStack(spacing:10) {
            Card(flex:1) Card(flex:6,visible:$middle) Card(flex:1)
        }
    )"),Shape);
    assert(hidden_flex.SetViewport({100,20}));assert(hidden_flex.SetBinding("middle",false));
    assert(BuildAndCommit(hidden_flex));
    assert(hidden_flex.Bounds({1,1}).width==45 && hidden_flex.Bounds({3,1}).x==55);
    assert(hidden_flex.SetBinding("middle",true));assert(BuildAndCommit(hidden_flex));
    assert(hidden_flex.Bounds({1,1}).width==10 && hidden_flex.Bounds({2,1}).width==60);

    // Natural-width center anchoring adapts when one group disappears.
    runtime::Scene natural_center(runtime::ParseBlueprint(R"(
        Card { HStack(anchor:"center",height:20,spacing:10) {
            Icon("layers",width:20,height:20)
            Icon("rectangle",width:20,height:20,visible:$middle)
            Icon("error",width:20,height:20)
        } }
    )"),Shape);
    assert(natural_center.SetViewport({100,20}));assert(natural_center.AcceptsBinding("middle",true));
    assert(BuildAndCommit(natural_center));assert(natural_center.Bounds({1,1}).x==10);
    assert(natural_center.SetBinding("middle",false));assert(BuildAndCommit(natural_center));
    assert(natural_center.Bounds({1,1}).x==25 && natural_center.Bounds({1,1}).width==50);
}
