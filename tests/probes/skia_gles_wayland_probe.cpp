#include "prism/sdk/client_application.hpp"
#include <chrono>
#include <cerrno>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/eventfd.h>
#include <unistd.h>

namespace {
using Application=prism::sdk::ClientApplication;
using Stats=prism::sdk::ClientRenderStats;
using namespace std::chrono_literals;
void Require(bool condition,const char* detail) {if(!condition)throw std::runtime_error(detail);}
void Print(std::string_view scenario,const Stats& stats) {
    std::cout<<"scenario="<<scenario<<" build_calls="<<stats.scene_build_attempts<<" builds="<<stats.scene_builds
        <<" layouts="<<stats.scene_layouts<<" gpu_attempts="<<stats.gpu_render_attempts
        <<" gpu_successes="<<stats.gpu_render_successes<<" swap_attempts="<<stats.swap_attempts
        <<" swap_successes="<<stats.swap_successes<<" frame_done="<<stats.frame_callbacks_done
        <<" state="<<stats.surface_state_commits<<" pixels="<<stats.surface_pixel_commits
        <<" noops="<<stats.surface_noops<<" failures="<<stats.surface_submission_failures<<'\n'<<std::flush;
}
void Until(Application& app,std::function<bool()> condition,const char* detail) {
    const auto deadline=std::chrono::steady_clock::now()+5s;
    while(!condition()) {
        Require(std::chrono::steady_clock::now()<deadline,detail);
        Require(app.Pump(10),"client stopped while waiting for submission");
    }
}
void Drain(Application& app) {
    Until(app,[&]{return !app.FrameCallbackPending();},"pixel callback did not complete");
    if(app.HasPresentationFeedback())
        Until(app,[&]{return app.PresentationCount()>0;},"startup presentation did not complete");
    // Dispatch residual configure/feedback messages before measuring quiet work.
    for(int i=0;i<4;++i)Require(app.Pump(0),"client stopped while draining events");
}
void NoPixels(const Stats& before,const Stats& after,const char* detail) {
    Require(after.gpu_render_attempts==before.gpu_render_attempts &&
        after.swap_attempts==before.swap_attempts && after.surface_pixel_commits==before.surface_pixel_commits,detail);
}
void Quiet(Application& app,std::string_view scenario) {
    Drain(app);const auto before=app.GetRenderStats();
    const auto deadline=std::chrono::steady_clock::now()+350ms;
    while(std::chrono::steady_clock::now()<deadline)Require(app.Pump(25),"static client stopped");
    const auto after=app.GetRenderStats();Print(scenario,after);
    NoPixels(before,after,"static/no-op work rendered or swapped pixels");
    Require(after.scene_builds==before.scene_builds && after.scene_layouts==before.scene_layouts &&
        after.surface_state_commits==before.surface_state_commits,"static client rebuilt or recommitted state");
}
prism::contracts::ThemeSnapshot ProbeTheme() {
    prism::contracts::ThemeSnapshot theme;
    theme.id="submission-probe";theme.name="Submission probe";theme.generation=1;
    theme.colors={{"foreground",{235,240,250,255}}};
    return theme;
}
int Verify(const std::string& socket,const std::string& app_id) {
    const auto config=[&](std::string id){return prism::sdk::ClientConfig{socket,std::move(id),
        "SDK submission verification","/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",640,400};};
    Application app(config(app_id));
    auto theme=ProbeTheme();Require(app.ApplyTheme(theme),"initial theme rejected");
    Require(app.Open(R"(
        Card(backdropBlur:$blur,inputShape:$shape,background:$tint,clip:true,cornerRadius:12,padding:20) {
            Text($title,font:20,foreground:"@foreground")
        }
    )"),"probe UI failed to open");
    Require(app.SetBinding("blur",12.0),"blur binding rejected");
    Require(app.SetBinding("shape",std::string("bounds")),"input-shape binding rejected");
    Require(app.SetBinding("tint",prism::contracts::Color{30,40,55,180}),"tint binding rejected");
    Require(app.SetBinding("title",std::string("Initial pixels")),"title binding rejected");
    Until(app,[&]{return app.IsMapped() && app.GetRenderStats().swap_successes>0;},"first pixels missing");
    Require(app.GlRenderer().find("V3D")!=std::string::npos,"probe did not use V3D");
    Drain(app);Print("first-pixels",app.GetRenderStats());Quiet(app,"idle");
    const auto same_before=app.GetRenderStats();
    for(int i=0;i<20;++i) {
        Require(app.SetBinding("title",std::string("Initial pixels")),"same accepted binding rejected");
        Require(app.SetBinding("blur",12.0),"same blur binding rejected");
    }
    Require(app.ApplyTheme(theme),"same theme rejected");
    Require(app.Pump(0),"no-op pump failed");
    NoPixels(same_before,app.GetRenderStats(),"same value/theme rendered pixels");
    Quiet(app,"same-value");
    auto before=app.GetRenderStats();
    Require(app.SetBinding("blur",18.0),"metadata binding rejected");
    Until(app,[&]{return app.GetRenderStats().surface_state_commits>before.surface_state_commits;},"effect state was not committed");
    auto after=app.GetRenderStats();Print("state-only",after);
    NoPixels(before,after,"effect-only change rendered or swapped pixels");
    Require(after.scene_builds==before.scene_builds && after.scene_layouts==before.scene_layouts,"effect-only change rebuilt draw commands");
    // Request Pixels and stop as soon as submitted, before pumping its callback.
    before=app.GetRenderStats();
    Require(app.SetBinding("title",std::string("Pending pixels")),"pixel binding rejected");
    Require(app.Pump(0),"pending pixel pump failed");
    Require(app.GetRenderStats().swap_successes>before.swap_successes,"pixel update missing");
    Require(app.FrameCallbackPending(),"fixture failed to observe a pending pixel callback");
    before=app.GetRenderStats();
    Require(app.SetBinding("blur",22.0),"pending metadata binding rejected");
    Require(app.Pump(0),"pending state pump failed");
    after=app.GetRenderStats();Print("state-during-pixel-callback",after);
    Require(after.surface_state_commits>before.surface_state_commits,"pixel callback blocked state commit");
    NoPixels(before,after,"state during callback rendered pixels");
    Drain(app);
    before=app.GetRenderStats();
    Require(app.SetBinding("tint",prism::contracts::Color{55,35,30,180}),"recovery tint rejected");
    Until(app,[&]{return app.GetRenderStats().swap_successes>before.swap_successes;},"pixels did not resume after state commit");
    Drain(app);Print("pixels-after-metadata",app.GetRenderStats());
    before=app.GetRenderStats();
    auto identity=theme;identity.generation=2;identity.id="same-style";identity.name="Same style";
    Require(app.ApplyTheme(identity),"identity-only theme rejected");
    Require(app.ThemeGeneration()==2 && app.Pump(0),"theme identity was not retained");
    NoPixels(before,app.GetRenderStats(),"theme identity-only change rendered pixels");
    auto invalid=identity;invalid.generation=3;invalid.colors.clear();
    std::string diagnostic;
    Require(!app.ApplyTheme(invalid,&diagnostic) && !diagnostic.empty(),"missing theme reference was accepted");
    Require(app.ThemeGeneration()==2,"failed preflight changed current theme");
    Require(!app.ReplaceUi("UnsupportedWidget()"),"invalid replacement UI was accepted");
    Require(app.Pump(0),"preflight rejection stopped the existing UI");
    NoPixels(before,app.GetRenderStats(),"failed preflight submitted pixels");
    Quiet(app,"preflight-rejection");
    // A second ordinary production client changes the first client's BSP size.
    // This exercises real configure/resize without embedding WM code here.
    const int configured=app.ConfigureCount();before=app.GetRenderStats();
    Application peer(config(app_id+".resize-peer"));
    Require(peer.Open("Card(background:#223344FF) { Text(\"Resize peer\") }"),"resize peer failed to open");
    const auto resize_deadline=std::chrono::steady_clock::now()+5s;
    while(!peer.IsMapped() || app.ConfigureCount()==configured || app.GetRenderStats().swap_successes==before.swap_successes) {
        Require(std::chrono::steady_clock::now()<resize_deadline,"production BSP resize did not produce pixels");
        Require(peer.Pump(5) && app.Pump(5),"resize client stopped");
    }
    Drain(app);Print("bsp-resize",app.GetRenderStats());
    before=app.GetRenderStats();peer.Close();
    Until(app,[&]{return app.GetRenderStats().swap_successes>before.swap_successes;},"removing peer did not restore pixel size");
    Drain(app);Quiet(app,"resize-restored");
    // Two successive external wakes prove interruptible indefinite waiting and
    // a drained notification, without using periodic polling as a runtime wake.
    const int wake=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);Require(wake>=0,"wake eventfd failed");
    for(int round=0;round<2;++round) {
        before=app.GetRenderStats();pollfd descriptor{wake,POLLIN,0};
        const auto started=std::chrono::steady_clock::now();
        std::thread notify([&]{std::this_thread::sleep_for(120ms);const std::uint64_t one=1;Require(write(wake,&one,sizeof(one))==sizeof(one),"wake write failed");});
        unsigned pumps=0;
        while(!(descriptor.revents&POLLIN)){Require(++pumps<=4,"indefinite wait woke repeatedly without work");Require(app.Pump(-1,std::span(&descriptor,1)),"external wake pump failed");}
        notify.join();
        Require(std::chrono::steady_clock::now()-started>=70ms,"idle runtime failed to block");
        std::uint64_t value{};Require(read(wake,&value,sizeof(value))==sizeof(value) && value==1,"wake did not drain once");
        NoPixels(before,app.GetRenderStats(),"external wake submitted pixels");
    }
    close(wake);Print("interruptible-wait",app.GetRenderStats());app.Close();
    return 0;
}
}
int main(int argc,char** argv) {
    if(argc<3){std::cerr<<"usage: prism_skia_gles_wayland_probe <socket> <dsl-file|--verify-submission> [app-id]\n";return 2;}
    try {
        if(std::string_view(argv[2])=="--verify-submission")
            return Verify(argv[1],argc>3?argv[3]:"prism.skia.submission.probe");
        std::ifstream input(argv[2]);if(!input)return 2;
        std::string source(std::istreambuf_iterator<char>{input},{});
        Application app({argv[1],argc>3?argv[3]:"prism.skia.gles.probe","Prism Skia GLES DSL",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",640,400});
        app.OnAction([](std::string_view action){std::cout<<"action="<<action<<'\n';});
        if(!app.Open(source))return 4;
        for(int i=0;i<500 && !app.IsCloseRequested();++i){if(!app.Pump(20))break;if(i==120)app.SetSlot("title","Skia GLES + Wayland");}
        std::cout<<"GL renderer="<<app.GlRenderer()<<"\nconfigure="<<app.ConfigureCount()<<" frame="<<app.FrameDoneCount()
            <<" presented="<<app.PresentedCount()<<" images="<<app.LoadedImageCount()<<'/'<<app.RequestedImageCount()<<'\n';
        Print("final",app.GetRenderStats());
        return app.GlRenderer().find("V3D")!=std::string::npos && app.IsMapped() && app.FrameDoneCount()>0 &&
            app.PresentedCount()>0 && app.LoadedImageCount()==app.RequestedImageCount()?0:5;
    } catch(const std::exception& error){std::cerr<<"GLES submission probe failed: "<<error.what()<<'\n';return 6;}
}
