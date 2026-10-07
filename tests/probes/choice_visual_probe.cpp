#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/runtime/text_shaper.hpp"
#include "prism/sdk/module_session.hpp"
#include "prism/theme/compiler.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>

using namespace prism;

namespace {
constexpr int height = 384;
constexpr auto font = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";

std::string Read(const std::filesystem::path &path)
{
    std::ifstream input(path);
    assert(input);
    return {std::istreambuf_iterator<char>(input), {}};
}

void Write(const std::filesystem::path &path, const std::vector<std::uint32_t> &pixels, int width)
{
    std::ofstream output(path, std::ios::binary);
    output << "P6\n" << width << ' ' << height << "\n255\n";
    for (auto pixel : pixels) {
        const char rgb[]{static_cast<char>(pixel >> 16), static_cast<char>(pixel >> 8),
                         static_cast<char>(pixel)};
        output.write(rgb, 3);
    }
    assert(output.good());
}

void Check(const std::filesystem::path &root, const std::filesystem::path &module_path,
           const std::filesystem::path &output, std::string_view material, std::string_view scheme,
           int width)
{
    runtime::TextShaper shaper(font);
    render_skia::RasterRenderer renderer(font);
    assert(shaper.Ready() && renderer.Ready());
    const auto shape = std::bind_front(
        static_cast<runtime::ShapedText (runtime::TextShaper::*)(std::string_view, double)>(
            &runtime::TextShaper::Shape),
        &shaper);
    runtime::Scene scene(
        runtime::ParseBlueprint(Read(root / "tests/fixtures/interface-system/choices.prism")),
        shape, shaper.FontId(), theme::LoadTheme(root / "resources/themes", material, 1, scheme));
    scene.SetViewport({static_cast<double>(width), height});
    sdk::ModuleSession module(module_path, "choice.fixture", 1,
                              std::bind_front(&runtime::Scene::SetBinding, &scene));
    assert(module.Start());
    auto list = scene.Build({1});
    assert(list);
    std::vector<std::uint32_t> before(width * height), after(width * height);
    assert(renderer.Render(*list, before.data(), width, height, width * 4));

    scene.HandleInput(
        contracts::KeyEvent{{1}, 0x2b, contracts::ButtonState::Pressed, false, 1, {1, 2, 1}, {}},
        scene.InputGeometry());
    auto radio = scene.HandleInput(
        contracts::KeyEvent{{1}, 0x4f, contracts::ButtonState::Pressed, false, 1, {1, 2, 1}, {}},
        scene.InputGeometry());
    assert(radio.control_edit && radio.control_edit->action == "scheme");
    module.ControlValue(*radio.control_edit);
    assert(scene.State(radio.control_edit->node).selected);
    scene.HandleInput(
        contracts::KeyEvent{{1}, 0x2b, contracts::ButtonState::Pressed, false, 1, {1, 2, 1}, {}},
        scene.InputGeometry());
    auto segment = scene.HandleInput(
        contracts::KeyEvent{{1}, 0x4f, contracts::ButtonState::Pressed, false, 1, {1, 2, 1}, {}},
        scene.InputGeometry());
    assert(segment.control_edit && segment.control_edit->action == "mode");
    module.ControlValue(*segment.control_edit);
    assert(scene.State(segment.control_edit->node).selected);

    list = scene.Build({1});
    assert(list && renderer.Render(*list, after.data(), width, height, width * 4));
    const auto rect = scene.Bounds(segment.control_edit->node);
    assert(rect.width >= 100 && rect.height >= 44);
    const auto pixel = static_cast<int>(rect.y + 10) * width + static_cast<int>(rect.x + 10);
    assert(before[pixel] != after[pixel]);

    if (!output.empty()) {
        const auto name =
            std::string(material) + "-" + std::string(scheme) + "-" + std::to_string(width);
        Write(output / (name + "-initial.ppm"), before, width);
        Write(output / (name + "-selected.ppm"), after, width);
    }
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 3 || argc == 4);
    const std::filesystem::path output = argc == 4 ? argv[3] : "";
    if (!output.empty()) {
        std::filesystem::create_directories(output);
    }
    for (const auto material : {"glass", "translucent", "transparent", "square"}) {
        for (const auto scheme : {"dark", "light"}) {
            for (const auto width : {320, 640}) {
                Check(argv[1], argv[2], output, material, scheme, width);
            }
        }
    }
}
