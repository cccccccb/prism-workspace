#include "prism/sdk/module_session.hpp"
#include <cassert>
#include <map>
#include <string>
using namespace prism;
int main(int argc,char** argv) {
    assert(argc==5);
    for (int n=1;n<5;++n) {
        std::map<std::string,runtime::PropertyValue> bindings;
        std::string launched;
        std::string selected_theme;
        std::uint64_t theme_request{};
        sdk::ModuleSession module(argv[n],"test",22,[&](auto key,auto value) {
            bindings[std::string(key)]=std::move(value); return true;
        },[&](auto app) { launched=app; return 6; },[] { return 5; },
            [&](auto id) { selected_theme=id; return ++theme_request; });
        assert(module.Start() && module.BackendReady());
        if (n==3) {
            assert(std::get<std::string>(bindings.at("running_badge"))=="0 Active");
            assert(std::get<double>(bindings.at("music_running"))==0);
            assert(std::get<double>(bindings.at("settings_running"))==0);
            module.Deliver(contracts::InstanceUpdate{{5},{8},42,contracts::InstanceChange::Running,"demo_player"});
            module.Deliver(contracts::InstanceUpdate{{5},{8},42,contracts::InstanceChange::Running,"demo_player"});
            assert(std::get<std::string>(bindings.at("running_badge"))=="1 Active");
            assert(std::get<double>(bindings.at("music_running"))==1);
            module.Action("app:launch:music"); assert(launched=="demo_player");
            assert(std::get<std::string>(bindings.at("launch_status"))=="Opening Music");
            module.Deliver(contracts::LaunchEvent{{6},{8},42,contracts::LaunchMilestone::Accepted});
            assert(std::get<std::string>(bindings.at("launch_status"))=="Opening Music");
            module.Deliver(contracts::LaunchEvent{{6},{8},42,contracts::LaunchMilestone::Activated});
            assert(std::get<std::string>(bindings.at("launch_status"))=="Music active");
            module.Action("app:launch:settings"); assert(launched=="demo_settings");
            module.Deliver(contracts::LaunchEvent{{6},{9},43,contracts::LaunchMilestone::BackendReady});
            assert(std::get<std::string>(bindings.at("launch_status"))=="Opening Pref");
            module.Deliver(contracts::LaunchEvent{{6},{9},43,contracts::LaunchMilestone::FirstPresented});
            assert(std::get<std::string>(bindings.at("launch_status"))=="Pref ready");
            module.Deliver(contracts::InstanceUpdate{{5},{8},42,contracts::InstanceChange::Stopped,"demo_player"});
            assert(std::get<std::string>(bindings.at("running_badge"))=="0 Active");
            assert(std::get<double>(bindings.at("music_running"))==0);
            module.Deliver(contracts::InstanceUpdate{{5},{9},43,contracts::InstanceChange::Running,"demo_player"});
            module.Deliver(contracts::InstanceUpdate{{5},{10},44,contracts::InstanceChange::Running,"demo_player"});
            module.Deliver(contracts::InstanceUpdate{{5},{9},43,contracts::InstanceChange::Stopped,"demo_player"});
            assert(std::get<double>(bindings.at("music_running"))==1); // Another real Music instance remains.
            module.Deliver(contracts::InstanceUpdate{{5},{10},44,contracts::InstanceChange::Stopped,"demo_player"});
            module.Deliver(contracts::InstanceUpdate{{5},{9},43,contracts::InstanceChange::Running,"demo_settings"});
            assert(std::get<double>(bindings.at("settings_running"))==1);
            module.Disconnected(); assert(std::get<std::string>(bindings.at("running_badge"))=="0 Active");
            assert(std::get<double>(bindings.at("settings_running"))==0);
        }
        if (n==4) {
            auto memory=std::get<std::string>(bindings.at("mem_usage_text")); assert(memory.find("GiB")!=std::string::npos);
            assert(!bindings.contains("window_tint") && !bindings.contains("theme_dark"));
            assert(theme_request==1 && selected_theme.empty());
            assert(std::get<double>(bindings.at("memory_usage"))>=0 && std::get<double>(bindings.at("memory_usage"))<=1);
            module.Deliver(contracts::ThemeEvent{1,1,contracts::ThemeStatus::Current,"glass","Prism Glass"});
            assert(std::get<std::string>(bindings.at("theme_status"))=="Theme: Prism Glass");
            module.Action("theme:transparent");
            assert(selected_theme=="transparent" && theme_request==2);
            assert(std::get<std::string>(bindings.at("theme_status"))=="Applying theme");
            module.Deliver(contracts::ThemeEvent{2,1,contracts::ThemeStatus::Rejected,"glass","Prism Glass","invalid"});
            assert(std::get<std::string>(bindings.at("theme_status"))=="Theme change rejected");
            module.Action("theme:square");
            module.Deliver(contracts::ThemeEvent{3,2,contracts::ThemeStatus::Applied,"square","Prism Square"});
            assert(std::get<std::string>(bindings.at("theme_status"))=="Theme: Prism Square");
            module.Deliver(contracts::ThemeEvent{0,1,contracts::ThemeStatus::Current,"glass","Prism Glass"});
            assert(std::get<std::string>(bindings.at("theme_status"))=="Theme: Prism Square");
            module.Action("sys:refresh");
        }
    }
}
