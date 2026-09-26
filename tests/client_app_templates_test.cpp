#include "prism/runtime/dsl_frontend.hpp"
#include "prism/sdk/module_session.hpp"
#include "prism/theme/compiler.hpp"
#include <cassert>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

struct ActionNode {
    prism::contracts::NodeId node;
    std::string action;
};
void CollectActions(const prism::runtime::Blueprint& blueprint,std::uint64_t& id,
                    std::vector<ActionNode>& actions) {
    const auto own=id++;
    for (const auto& property:blueprint.properties)
        if (property.id==prism::runtime::DslProperty::Action)
            actions.push_back({prism::contracts::NodeId{static_cast<std::uint32_t>(own),1},
                std::get<std::string>(property.value)});
    for (const auto& child:blueprint.children) CollectActions(child,id,actions);
}
void CheckActions(prism::runtime::Scene& scene,const std::vector<ActionNode>& actions,double width,double height) {
    for (const auto& target:actions) {
        const auto bounds=scene.Bounds(target.node);
        assert(bounds.width>0 && bounds.height>0);
        assert(bounds.x>=0 && bounds.y>=0 && bounds.x+bounds.width<=width+0.001 &&
            bounds.y+bounds.height<=height+0.001);
        assert(scene.ActionAt({bounds.x+bounds.width/2,bounds.y+bounds.height/2})==target.action);
    }
}

int main(int argc,char** argv) {
    struct Template { const char* path; double width,height; };
    const Template cases[]{
        {"prism-desktop/ui/desktop.prism",1024,600},
        {"prism-topbar/ui/topbar.prism",1024,52},
        {"prism-dock/ui/dock.prism",620,100},
        {"demos/demo_player/master.prism",482,420},
        {"demos/demo_settings/master.prism",482,420},
        {"demos/demo_player/preview.prism",482,420}};
    for (unsigned n=0;n<std::size(cases);++n) {
        const auto& item=cases[n];
        std::ifstream file(std::string(PRISM_SOURCE_ROOT) + "/" + item.path);
        assert(file);
        std::string source(std::istreambuf_iterator<char>{file}, {});
        auto blueprint = prism::runtime::ParseBlueprint(source,
            [](std::string_view) { return prism::contracts::ResourceId{12}; });
        std::uint64_t id{};
        std::vector<ActionNode> actions;
        CollectActions(blueprint,id,actions);
        prism::runtime::Scene scene(std::move(blueprint),
            [](std::string_view text, double font) {
                return prism::runtime::ShapedText{{},text.size()*font*0.5,font};
            },
            prism::contracts::ResourceId{1},prism::theme::LoadTheme(prism::theme::DefaultThemeRoot(),"glass"));
        assert(scene.SetViewport({item.width,item.height}));
        if (argc==6 && n<5) {
            // Business startup must satisfy the actual template binding schema,
            // including typed progress, color and icon properties.
            std::uint64_t theme_request=7;
            prism::sdk::ModuleSession module(argv[n+1],"template_test",42,
                [&](auto key,auto value) {
                    if (!scene.AcceptsBinding(key,value)) return false;
                    scene.SetBinding(key,std::move(value)); return true;
                },[](auto) { return 6; },[] { return 5; },[&](auto) { return theme_request++; });
            assert(module.Start() && module.BackendReady());
            module.Action(n==3?"player:toggle":n==4?"theme:transparent":"ignored");
            if (n==4) module.Deliver(prism::contracts::ThemeEvent{0,0,prism::contracts::ThemeStatus::Current,"glass","Prism Glass"});
            module.Tick(prism::sdk::MonotonicNs()+1000000000ULL);
            if (n==2) module.Deliver(prism::contracts::InstanceUpdate{
                {5},{8},42,prism::contracts::InstanceChange::Running,"demo_player"});
        }
        assert(scene.Build(prism::contracts::WindowId{1}));
        CheckActions(scene,actions,item.width,item.height);
        std::uint64_t generation=1;
        for (const auto* theme:{"translucent","transparent","square","glass"}) {
            assert(scene.ApplyTheme(prism::theme::LoadTheme(prism::theme::DefaultThemeRoot(),theme,generation++)));
            assert(scene.Build(prism::contracts::WindowId{1}));
            CheckActions(scene,actions,item.width,item.height);
            assert(!scene.InputRegions().empty());
        }
        if (n==3 || n==4) for (const auto size: {prism::contracts::LogicalSize{482,204},
                                               prism::contracts::LogicalSize{244,420}}) {
            assert(scene.SetViewport(size));
            assert(scene.Build(prism::contracts::WindowId{1}));
            CheckActions(scene,actions,size.width,size.height);
        }
    }
}
