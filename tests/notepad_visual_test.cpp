#include "prism/animation/timeline.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/load_plan.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/runtime/text_shaper.hpp"
#include "prism/theme/compiler.hpp"
#include "prism/theme/motion_compiler.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>

using namespace prism;

namespace {
class Clock final : public animation::AnimationClock {
public:
    std::uint64_t now{1'000'000'000};

    std::uint64_t NowNs() const noexcept override
    {
        return now;
    }
};

contracts::LogicalPoint Target(runtime::Scene &scene, std::string_view action)
{
    for (const auto &node : scene.InputGeometry()->nodes) {
        if (node.visible && node.action == action) {
            const auto b = node.bounds;
            assert(b.width > 0 && b.height > 0 && b.x >= 0 && b.y >= 0);
            const auto viewport = scene.InputGeometry()->viewport;
            assert(b.x + b.width <= viewport.width && b.y + b.height <= viewport.height);
            const contracts::LogicalPoint center{b.x + b.width / 2, b.y + b.height / 2};
            if (scene.ActionAt(center) != action) {
                std::cerr << "Hit mismatch " << action << " at " << center.x << "," << center.y
                          << " got " << scene.ActionAt(center).value_or("<none>") << "\n";
            }
            assert(scene.ActionAt(center) == action);
            return center;
        }
    }
    assert(false && "Missing visible action");
    return {};
}

std::optional<contracts::DisplayList> Settle(runtime::Scene &scene, Clock &clock)
{
    // Visibility changes rebuild input geometry and may clear hover/focus, starting feedback.
    auto initial = scene.Build({1});
    clock.now += 1'000'000'000;
    scene.AdvanceAnimations(clock.now);
    auto list = scene.Build({1});
    assert(!scene.HasActiveAnimations());
    assert(!scene.Build({1}));
    return list ? list : initial;
}

std::string Read(const std::filesystem::path &path)
{
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), {}};
}

void WritePpm(const std::filesystem::path &path, const std::vector<std::uint8_t> &pixels, int width,
              int height)
{
    std::ofstream file(path, std::ios::binary);
    file << "P6\n" << width << ' ' << height << "\n255\n";
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        const char rgb[] = {static_cast<char>(pixels[i + 2]), static_cast<char>(pixels[i + 1]),
                            static_cast<char>(pixels[i])};
        file.write(rgb, 3);
    }
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 3);
    const std::filesystem::path root = argv[1], output = argv[2];
    std::filesystem::create_directories(output);
    const auto package = root / "prism-notepad";
    const auto plan = runtime::CompileLoadPlan(Read(package / "master.prism"),
                                               {"master", "master.prism", "test"}, package);
    const auto layout = runtime::PrepareLayout(Read(package / "layout.prism"), plan);
    std::vector<runtime::PreparedUnit> units;
    for (const auto &name : {"toolbar", "editor", "status"}) {
        units.push_back(
            {name, runtime::PrepareComponent(Read(package / "ui" / (std::string(name) + ".prism")),
                                             {name, name, "test"})});
    }
    const auto composed = runtime::ComposeCritical(plan, layout, units);
    const std::string font = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    runtime::TextShaper shaper(font);
    render_skia::RasterRenderer renderer(font);
    assert(shaper.Ready() && renderer.Ready());
    for (const auto &theme : {"glass", "square", "translucent", "transparent"}) {
        for (const auto &scheme : {"light", "dark"}) {
            for (int width : {244, 320, 482, 900}) {
                const int height = width == 244   ? 420
                                   : width == 320 ? 480
                                   : width == 482 ? 204
                                                  : 640;
                const auto snapshot = theme::LoadTheme(root / "resources/themes", theme, 1, scheme);
                runtime::Scene scene(
                    runtime::LinkComponent(composed),
                    std::bind_front(static_cast<runtime::ShapedText (runtime::TextShaper::*)(
                                        std::string_view, double)>(&runtime::TextShaper::Shape),
                                    &shaper),
                    shaper.FontId(), snapshot);
                Clock clock;
                for (const auto &binding : plan.bindings) {
                    scene.SetBinding(binding.name, binding.initial);
                }
                scene.SetBinding("active_0", true);
                scene.SetBinding("tab_0", true);
                scene.SetBinding("tab_1", true);
                scene.SetBinding("label_0", std::string("Welcome.txt"));
                scene.SetBinding("label_1", std::string("● Draft.txt"));
                scene.SetBinding("selected_0", true);
                scene.SetBinding("pages", true);
                scene.SetBinding("title", std::string("Welcome.txt"));
                scene.SetBinding("path", std::string("/home/you/Documents/Welcome.txt"));
                scene.SetBinding("details", std::string("2/8 documents · 186 bytes · 8 lines"));
                scene.SetBinding("status", std::string("Saved"));
                scene.SetBinding("summary", std::string("8 lines · UTF-8"));
                scene.SetBinding(
                    "text_0",
                    std::string(
                        "A quiet place to write.\n\nMake a note. Keep an idea.\n\n- Open a text "
                        "file\n- Edit several documents\n- Save when you are ready\n"));
                scene.EnableAnimations(&clock);
                scene.SetViewport({static_cast<double>(width), static_cast<double>(height)});
                const auto list = scene.Build({1});
                assert(list);
                for (const auto &action :
                     {"new", "open-panel", "save", "page-previous", "page-next", "documents",
                      "select:0", "close:0", "select:1", "close:1", "edit:0"}) {
                    Target(scene, action);
                }
                std::vector<std::uint8_t> pixels(width * height * 4);
                assert(renderer.Render(*list, pixels.data(), width, height, width * 4));
                WritePpm(output / (std::string(theme) + "-" + scheme + "-" + std::to_string(width) +
                                   ".ppm"),
                         pixels, width, height);
                assert(!scene.Build({1}));
                const auto save = Target(scene, "save");
                scene.HandleInput(contracts::PointerMotionEvent{{1}, save});
                assert(scene.HasActiveAnimations());
                clock.now += 60'000'000;
                scene.AdvanceAnimations(clock.now);
                assert(scene.Build({1}));
                assert(scene.ActionAt(save) == "save");
                contracts::PointerButtonEvent button;
                button.position = save;
                button.state = contracts::ButtonState::Pressed;
                scene.HandleInput(button);
                Settle(scene, clock);
                assert(scene.ActionAt(save) == "save");
                button.state = contracts::ButtonState::Released;
                const auto release = scene.HandleInput(button);
                assert(release.activation && release.activation->action == "save");
                Settle(scene, clock);

                contracts::KeyEvent tab;
                tab.physical_key = 0x2b;
                tab.state = contracts::ButtonState::Pressed;
                scene.HandleInput(tab);
                assert(scene.HasActiveAnimations());
                Settle(scene, clock);

                scene.SetBinding("active_0", false);
                for (int i = 0; i < 8; ++i) {
                    scene.SetBinding("used_" + std::to_string(i), true);
                    scene.SetBinding("label_" + std::to_string(i),
                                     std::string("Document ") + std::to_string(i + 1));
                }
                scene.SetBinding("label_0", std::string("Welcome.txt"));
                scene.SetBinding("selected_0", true);
                scene.SetBinding("documents", true);
                scene.SetBinding("normal", false);
                scene.SetBinding("list_opacity", 1.0);
                scene.SetBinding("list_offset", 0.0);
                Settle(scene, clock);
                Target(scene, "save-as");
                for (int i = 0; i < 8; ++i) {
                    Target(scene, "select:" + std::to_string(i));
                }
                scene.SetBinding("documents", false);
                scene.SetBinding("active_0", true);
                scene.Build({1});
                const auto editor = Target(scene, "edit:0");
                scene.SetBinding("active_0", false);
                scene.SetBinding("confirm", true);
                scene.SetBinding("confirm_title", std::string("Save changes to Draft.txt?"));
                scene.SetBinding("confirm_opacity", 1.0);
                scene.SetBinding("confirm_offset", 0.0);
                const auto prompt = Settle(scene, clock);
                assert(prompt);
                if (std::string_view(theme) == "glass") {
                    assert(renderer.Render(*prompt, pixels.data(), width, height, width * 4));
                    WritePpm(output / (std::string(theme) + "-" + scheme + "-" +
                                       std::to_string(width) + "-confirm.ppm"),
                             pixels, width, height);
                }
                Target(scene, "cancel");
                Target(scene, "discard");
                Target(scene, "save-close");
                assert(scene.ActionAt(editor) != "edit:0");
                scene.SetBinding("confirm", false);
                scene.SetBinding("path_panel", true);
                scene.SetBinding("path_title", std::string("Save document"));
                scene.Build({1});
                Target(scene, "path");
                Target(scene, "cancel");
                Target(scene, "apply-path");
                scene.SetBinding("path_panel", false);
                scene.SetBinding("active_0", true);
                scene.SetBinding("normal", true);
                scene.Build({1});
                Target(scene, "edit:0");

                auto instant = snapshot;
                instant.generation++;
                instant.motion = theme::LoadMotion(root / "resources/motions", "instant");
                assert(scene.ApplyTheme(instant));
                scene.HandleInput(contracts::PointerMotionEvent{{1}, {0, 0}});
                scene.HandleInput(contracts::PointerMotionEvent{{1}, save});
                assert(!scene.HasActiveAnimations());
            }
        }
    }
}
