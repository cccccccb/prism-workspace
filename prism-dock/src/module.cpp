#include "prism/app/module_support.hpp"
#include "prism/contracts/launch.hpp"
#include <map>
#include <string>
namespace {
struct Dock {
    const PrismHostApiV1* host;
    std::map<std::uint64_t,std::string> windows;
    std::uint64_t last_launch{};
    bool presented{}, ready{};
    void Update() {
        bool music=false,settings=false;
        for (const auto& [id,app]:windows) {
            music=music || app=="demo_player"; settings=settings || app=="demo_settings";
        }
        prism::app::Text(host,"running_badge",std::to_string(windows.size())+" Active");
        prism::app::Number(host,"music_running",music?1:0);
        prism::app::Number(host,"settings_running",settings?1:0);
    }
};
void* Create(const PrismAppInitV1* init) noexcept {
    if (!prism::app::ValidHost(init) || init->host->struct_size<sizeof(PrismHostApiV1) ||
        !init->host->launch_app || !init->host->subscribe_instances) return nullptr;
    Dock* dock=nullptr;
    try {
        dock=new Dock{init->host,{}}; dock->Update();
        if (!prism::app::Text(init->host,"launch_status","")) { delete dock; return nullptr; }
        if (init->host->subscribe_instances(init->host->context) && prism::app::Ready(init->host)) return dock;
    } catch (...) {} delete dock; return nullptr;
}
void Destroy(void* instance) noexcept { delete static_cast<Dock*>(instance); }
void Action(void* instance,PrismStringViewV1 value) noexcept {
    try {
        auto& dock=*static_cast<Dock*>(instance); std::string_view action(value.data,value.size), app;
        if (action=="app:launch:music") app="demo_player";
        else if (action=="app:launch:settings") app="demo_settings"; else return;
        dock.last_launch=dock.host->launch_app(dock.host->context,{app.data(),app.size()});
        dock.presented=dock.ready=false;
        prism::app::Text(dock.host,"launch_status",dock.last_launch
            ? (app=="demo_player" ? "Opening Music" : "Opening Pref") : "Launch failed");
    } catch (...) {}
}
void Instances(void* instance,const PrismInstanceEventV1* event) noexcept {
    try {
        auto& dock=*static_cast<Dock*>(instance);
        if (event->change==0) dock.windows.clear();
        else if (event->change==1) dock.windows[event->instance_id]=std::string(event->app_id.data,event->app_id.size);
        else if (event->change==2) dock.windows.erase(event->instance_id);
        dock.Update();
    } catch (...) {}
}
void LaunchEvent(void* instance,const PrismLaunchEventV1* event) noexcept {
    try {
        auto& dock=*static_cast<Dock*>(instance);
        if (event->request_id!=dock.last_launch) return;
        using prism::contracts::LaunchMilestone;
        const auto milestone=static_cast<LaunchMilestone>(event->milestone);
        if (event->error_code) {
            prism::app::Text(dock.host,"launch_status","Launch failed");
            return;
        }
        if (milestone==LaunchMilestone::FirstPresented) dock.presented=true;
        if (milestone==LaunchMilestone::BackendReady) dock.ready=true;
        const std::string_view app(event->app_id.data,event->app_id.size);
        if (milestone==LaunchMilestone::Activated)
            prism::app::Text(dock.host,"launch_status",app=="demo_player" ? "Music active" : "Pref active");
        else if (dock.presented && dock.ready)
            prism::app::Text(dock.host,"launch_status",app=="demo_player" ? "Music ready" : "Pref ready");
    } catch (...) {}
}
const PrismAppModuleV1 api{sizeof(api),PRISM_APP_ABI_V1,Create,Destroy,Action,nullptr,LaunchEvent,Instances};
}
extern "C" PRISM_APP_EXPORT const PrismAppModuleV1* prism_app_module_v1() { return &api; }
