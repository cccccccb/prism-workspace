#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/runtime/text_shaper.hpp"
#include "prism/theme/compiler.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
using namespace prism;

int main(int argc, char **argv)
{
    assert(argc >= 2);
    const std::filesystem::path root(argv[1]);
    std::ifstream file(root / "tests/fixtures/interface-system/menu.prism");
    const std::string source{std::istreambuf_iterator<char>(file), {}};
    constexpr auto font = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    runtime::TextShaper shaper(font);
    render_skia::RasterRenderer renderer(font);
    const auto shape = std::bind_front(
        static_cast<runtime::ShapedText (runtime::TextShaper::*)(std::string_view, double)>(
            &runtime::TextShaper::Shape),
        &shaper);
    for (auto material : {"glass", "translucent", "transparent", "square"}) {
        for (auto scheme : {"light", "dark"}) {
            for (int width : {320, 640}) {
                const int height = width == 320 ? 280 : 420;
                runtime::Scene scene(
                    runtime::ParseBlueprint(source), shape, shaper.FontId(),
                    theme::LoadTheme(root / "resources/themes", material, 1, scheme));
                scene.SetViewport({double(width), double(height)});
                auto list = scene.Build({1});
                assert(list);
                contracts::NodeId anchor;
                for (const auto &node : scene.InputGeometry()->nodes) {
                    if (node.action == "files") {
                        anchor = node.id;
                    }
                }
                assert(anchor);
                std::vector<std::uint32_t> before(width * height), after(width * height),
                    child(width * height);
                assert(renderer.Render(*list, before.data(), width, height, width * 4));
                assert(scene.OpenPopup(anchor));
                list = scene.Build({1});
                assert(list && renderer.Render(*list, after.data(), width, height, width * 4));
                assert(before != after);
                contracts::NodeId sort;
                for (const auto &item : scene.InputGeometry()->nodes) {
                    if (item.action == "sort") {
                        sort = item.id;
                    }
                }
                assert(sort && scene.OpenPopup(sort));
                list = scene.Build({1});
                assert(list && renderer.Render(*list, child.data(), width, height, width * 4));
                assert(child != after);
                if (argc > 2) {
                    std::filesystem::create_directories(argv[2]);
                    for (int state = 0; state < 3; ++state) {
                        auto name = std::string(material) + "-" + scheme + "-" +
                                    std::to_string(width) +
                                    (state == 2 ? "-child.ppm"
                                     : state    ? "-open.ppm"
                                                : "-closed.ppm");
                        std::ofstream output(std::filesystem::path(argv[2]) / name,
                                             std::ios::binary);
                        output << "P6\n" << width << ' ' << height << "\n255\n";
                        for (auto pixel : state == 2 ? child : state ? after : before) {
                            const char rgb[]{char(pixel >> 16), char(pixel >> 8), char(pixel)};
                            output.write(rgb, 3);
                        }
                        assert(output.good());
                    }
                }
            }
        }
    }
}
