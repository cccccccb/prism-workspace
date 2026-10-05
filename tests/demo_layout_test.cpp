#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/load_plan.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/runtime/text_shaper.hpp"
#include "prism/theme/compiler.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>

using namespace prism;

namespace {
std::string Read(const std::filesystem::path &path)
{
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), {}};
}

contracts::ResourceId ResolveImage(std::string_view)
{
    return {12};
}

void Check(runtime::Scene &scene)
{
    assert(scene.Build({1}));
    const auto geometry = scene.InputGeometry();
    for (const auto &node : geometry->nodes) {
        if (node.visible && node.parent) {
            const auto *parent = geometry->Find(node.parent);
            assert(parent);
            const auto b = node.bounds;
            const auto p = parent->bounds;
            // A group must budget for its contents, even when its controls still hit.
            if (b.x < p.x - 0.5 || b.y < p.y - 0.5 || b.x + b.width > p.x + p.width + 0.5 ||
                b.y + b.height > p.y + p.height + 0.5) {
                std::cerr << "Content outside group: node=" << node.id.index
                          << " parent=" << node.parent.index << " action=" << node.action
                          << " bounds=" << b.x << ',' << b.y << ',' << b.width << ',' << b.height
                          << " parent=" << p.x << ',' << p.y << ',' << p.width << ',' << p.height
                          << '\n';
                assert(false);
            }
        }
        if (!node.visible || node.action.empty()) {
            continue;
        }
        const auto b = node.bounds;
        if (b.width < 24 || b.height < 24 || b.x < 0 || b.y < 0 ||
            b.x + b.width > geometry->viewport.width ||
            b.y + b.height > geometry->viewport.height ||
            scene.ActionAt({b.x + b.width / 2, b.y + b.height / 2}) != node.action) {
            std::cerr << "Unreachable control: " << node.action << '\n';
            assert(false);
        }
    }
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2);
    const std::filesystem::path root = argv[1];
    runtime::TextShaper shaper("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    const auto shape = std::bind_front(
        static_cast<runtime::ShapedText (runtime::TextShaper::*)(std::string_view, double)>(
            &runtime::TextShaper::Shape),
        &shaper);
    const auto package = root / "demos/demo_player";
    const auto plan = runtime::CompileLoadPlan(Read(package / "master.prism"),
                                               {"master", "master.prism", "test"}, package);
    const auto layout = runtime::PrepareLayout(Read(package / "layout.prism"), plan);
    std::vector<runtime::PreparedUnit> units;
    std::vector<runtime::RegionUpdate> deferred;
    for (const auto &unit : plan.components) {
        auto prepared = runtime::PrepareComponent(Read(package / unit.source_path),
                                                  {unit.id, unit.source_path.string(), "test"});
        if (unit.phase == runtime::LoadPhase::Deferred) {
            deferred.push_back({unit.id, runtime::LinkComponent(prepared, ResolveImage)});
        }
        units.push_back({unit.id, std::move(prepared)});
    }
    const auto composed = runtime::ComposeCritical(plan, layout, units);
    for (const auto &theme : {"glass", "square", "translucent", "transparent"}) {
        for (const auto &scheme : {"light", "dark"}) {
            const auto snapshot = theme::LoadTheme(root / "resources/themes", theme, 1, scheme);
            for (const auto size : {contracts::LogicalSize{244, 420},
                                    {482, 204},
                                    {482, 420},
                                    {409, 540},
                                    {244, 540},
                                    {620, 540},
                                    {600, 540},
                                    {900, 640}}) {
                for (const auto &page : {"performance", "appearance", "system"}) {
                    runtime::Scene scene(
                        runtime::ParseBlueprint(Read(root / "demos/demo_settings/master.prism"),
                                                ResolveImage),
                        shape, shaper.FontId(), snapshot);
                    scene.SetViewport(size);
                    for (const auto &other : {"performance", "appearance", "system"}) {
                        scene.SetBinding(std::string("page_") + other, false);
                    }
                    scene.SetBinding("theme_error_visible", false);
                    scene.SetBinding("theme_normal", true);
                    scene.SetBinding(std::string("page_") + page, true);
                    scene.SetBinding("cpu_usage_text", std::string("35%"));
                    scene.SetBinding("mem_usage_text", std::string("1.2 / 3.7 GiB"));
                    scene.SetBinding("gpu_usage_text", std::string("500 MHz"));
                    Check(scene);
                    scene.SetBinding("theme_normal", false);
                    scene.SetBinding("theme_error_visible", true);
                    scene.SetBinding("theme_status", std::string("Appearance unavailable"));
                    Check(scene);
                }
                runtime::Scene player(runtime::LinkComponent(composed, ResolveImage), shape,
                                      shaper.FontId(), snapshot);
                auto full = player.RegionBlueprint(deferred);
                runtime::Scene complete(std::move(full), shape, shaper.FontId(), snapshot);
                for (const auto &binding : plan.bindings) {
                    complete.SetBinding(binding.name, binding.initial);
                }
                complete.SetViewport(size);
                complete.SetBinding("library_empty", false);
                for (int row = 1; row <= 3; ++row) {
                    complete.SetBinding("library_row_" + std::to_string(row), true);
                    complete.SetBinding("library_title_" + std::to_string(row),
                                        std::string("Track"));
                }
                Check(complete);
                complete.SetBinding("catalog_error", true);
                complete.SetBinding("repeat_active", true);
                complete.SetBinding("favorite_active", false);
                Check(complete);
                complete.SetBinding("catalog_error", false);
                complete.SetBinding("artwork_visible", false);
                complete.SetBinding("library_visible", true);
                Check(complete);
                assert(!complete.Build({1}));
            }
        }
    }
}
