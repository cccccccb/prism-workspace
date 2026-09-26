#include "prism/runtime/dsl_frontend.hpp"
#include <cassert>
#include <fstream>
#include <iterator>
#include <string>

int main() {
    for (const char* path : {"prism-desktop/ui/desktop.prism", "prism-topbar/ui/topbar.prism",
                             "prism-dock/ui/dock.prism", "demos/demo_player/master.prism",
                             "demos/demo_settings/master.prism"}) {
        std::ifstream file(std::string(PRISM_SOURCE_ROOT) + "/" + path);
        assert(file);
        std::string source(std::istreambuf_iterator<char>{file}, {});
        auto blueprint = prism::runtime::ParseBlueprint(source,
            [](std::string_view) { return prism::contracts::ResourceId{12}; });
        prism::runtime::Scene scene(std::move(blueprint),
            [](std::string_view, double) { return prism::runtime::ShapedText{}; },
            prism::contracts::ResourceId{1});
        assert(scene.SetViewport({1280, 720}));
        assert(scene.Build(prism::contracts::WindowId{1}));
    }
}
