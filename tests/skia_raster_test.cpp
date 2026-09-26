#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include <cassert>
#include <cstdint>
#include <variant>
#include <vector>

int main() {
    prism::render_skia::RasterRenderer renderer("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    assert(renderer.Ready());
    auto shaped = renderer.Shape("Prism", 20);
    assert(shaped.glyphs.size() == 5 && shaped.width > 0);
    prism::runtime::Scene scene(prism::runtime::ParseBlueprint(
        "VStack(background: #102030FF) { Text(\"Prism\", font: 20, foreground: #FFFFFFFF) "
        "Card(height: 30, background: #FF0000FF).cornerRadius(5) }"),
        [&](std::string_view text, double size) { return renderer.Shape(text, size); }, renderer.FontId());
    assert(scene.SetViewport({120, 80}));
    auto list = scene.Build(prism::contracts::WindowId{1});
    assert(list);
    auto image = prism::render_skia::RasterRenderer::DecodePng(PRISM_TEST_IMAGE);
    assert(image && image->width == 4 && image->height == 4);
    assert(renderer.RegisterImage(prism::contracts::ResourceId{2}, *image));
    list->commands.emplace_back(prism::contracts::DrawImage{
        prism::contracts::ResourceId{2}, {90, 0, 4, 4}});
    std::vector<std::uint32_t> pixels(120 * 80);
    assert(renderer.Render(*list, pixels.data(), 120, 80, 120 * 4));
    // BGRA little-endian: opaque background and red card must both appear.
    assert(pixels[0] == 0xFF102030);
    bool red = false;
    for (auto pixel : pixels) if (pixel == 0xFFFF0000) red = true;
    assert(red);
    assert(pixels[36 * 120] == 0xFF102030); // rounded card corner remains background
    assert(pixels[90] == 0xFFFF0000); // PNG resource reached Skia drawImageRect
    auto invalid = *list;
    invalid.commands.push_back(prism::contracts::PopClip{});
    assert(!renderer.Render(invalid, pixels.data(), 120, 80, 120 * 4));
    assert(!renderer.Render(*list, pixels.data(), 0, 80, 120 * 4));
}
