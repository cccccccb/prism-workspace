#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/dsl_frontend.hpp"
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
constexpr int height = 600;
constexpr auto font = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";

std::string Read(const std::filesystem::path &file)
{
    std::ifstream stream(file);
    assert(stream);
    return {std::istreambuf_iterator<char>(stream), {}};
}

const runtime::InputSnapshotNode &Find(const runtime::InputSnapshot &snapshot,
                                       std::string_view action)
{
    for (const auto &node : snapshot.nodes) {
        if (node.action == action) {
            return node;
        }
    }
    throw std::runtime_error("Control action missing");
}

contracts::LogicalPoint Center(const runtime::InputSnapshotNode &node)
{
    return {node.bounds.x + node.bounds.width / 2, node.bounds.y + node.bounds.height / 2};
}

void Write(const std::filesystem::path &file, const std::vector<std::uint32_t> &pixels, int width)
{
    std::ofstream out(file, std::ios::binary);
    out << "P6\n" << width << ' ' << height << "\n255\n";
    for (auto pixel : pixels) {
        const char rgb[]{static_cast<char>(pixel >> 16), static_cast<char>(pixel >> 8),
                         static_cast<char>(pixel)};
        out.write(rgb, 3);
    }
    assert(out.good());
}

void Check(const std::filesystem::path &root, const std::filesystem::path &output,
           std::string_view material, std::string_view scheme, int width)
{
    const auto theme = theme::LoadTheme(root / "resources/themes", material, 1, scheme);
    runtime::TextShaper shaper(font);
    render_skia::RasterRenderer renderer(font);
    assert(shaper.Ready() && renderer.Ready());
    const auto shape = std::bind_front(
        static_cast<runtime::ShapedText (runtime::TextShaper::*)(std::string_view, double)>(
            &runtime::TextShaper::Shape),
        &shaper);
    runtime::Scene scene(
        runtime::ParseBlueprint(Read(root / "tests/fixtures/interface-system/controls.prism")),
        shape, shaper.FontId(), theme);
    scene.SetViewport({static_cast<double>(width), height});
    assert(scene.Build({1}));
    const auto snapshot = scene.InputGeometry();
    const auto &hover = Find(*snapshot, "hover");
    const auto &pressed = Find(*snapshot, "pressed");
    const auto &focus = Find(*snapshot, "focus");
    const auto &disabled = Find(*snapshot, "disabled");
    scene.HandleInput(contracts::PointerMotionEvent{{1}, Center(hover), 1, {1, 1, 1}});
    scene.HandleInput(contracts::PointerButtonEvent{{1},
                                                    Center(pressed),
                                                    contracts::PointerButton::Primary,
                                                    contracts::ButtonState::Pressed,
                                                    0,
                                                    1,
                                                    {1, 2, 1}});
    scene.HandleInput(
        contracts::KeyEvent{{1}, 0x2b, contracts::ButtonState::Pressed, false, 1, {1, 3, 1}, {}});
    assert(scene.State(hover.id).hovered && scene.State(pressed.id).pressed);
    assert(scene.State(focus.id).focusVisible && !scene.State(disabled.id).enabled);
    assert(!scene.HitTest(Center(disabled)));
    const auto list = scene.Build({1});
    assert(list);
    std::vector<std::uint32_t> pixels(width * height);
    assert(renderer.Render(*list, pixels.data(), width, height, width * 4));
    if (!output.empty()) {
        Write(output / (std::string(material) + "-" + std::string(scheme) + "-" +
                        std::to_string(width) + ".ppm"),
              pixels, width);
    }
    // State fills differ at an interior point clear of text and rounded corners.
    const auto color = [&pixels, width](const runtime::InputSnapshotNode &node) {
        return pixels[static_cast<int>(node.bounds.y + 12) * width +
                      static_cast<int>(node.bounds.x + 12)];
    };
    const auto &normal = Find(*snapshot, "default");
    assert(color(normal) != color(hover) && color(hover) != color(pressed));
    assert(color(normal) != color(disabled));
    std::cout << material << '/' << scheme << " control states passed\n";
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    const std::filesystem::path output = argc == 3 ? argv[2] : "";
    if (!output.empty()) {
        std::filesystem::create_directories(output);
    }
    for (const auto *material : {"glass", "translucent", "transparent", "square"}) {
        for (const auto *scheme : {"dark", "light"}) {
            Check(argv[1], output, material, scheme, 680);
            Check(argv[1], output, material, scheme, 380);
        }
    }
}
