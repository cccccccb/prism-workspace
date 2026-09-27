#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include <cassert>
#include <cstdint>
#include <variant>
#include <vector>

int main()
{
    prism::render_skia::RasterRenderer renderer("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    assert(renderer.Ready());
    const prism::contracts::ResourceId alternate_font{37};
    assert(
        renderer.RegisterFont(alternate_font, "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"));
    assert(
        !renderer.RegisterFont(alternate_font, "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"));
    assert(renderer.Shape(alternate_font, "Prism", 20).glyphs.size() == 5);
    auto shaped = renderer.Shape("Prism", 20);
    assert(shaped.glyphs.size() == 5 && shaped.width > 0);
    prism::runtime::Scene scene(
        prism::runtime::ParseBlueprint(
            "VStack(background: #102030FF) { Text(\"Prism\", font: 20, foreground: #FFFFFFFF) "
            "Card(height: 30, background: #FF0000FF).cornerRadius(5) }"),
        [&](std::string_view text, double size) { return renderer.Shape(text, size); },
        renderer.FontId());
    assert(scene.SetViewport({120, 80}));
    auto list = scene.Build(prism::contracts::WindowId{1});
    assert(list);
    auto image = prism::render_skia::RasterRenderer::DecodePng(PRISM_TEST_IMAGE);
    assert(image && image->width == 4 && image->height == 4);
    assert(renderer.RegisterImage(prism::contracts::ResourceId{2}, *image));
    list->commands.emplace_back(
        prism::contracts::DrawImage{prism::contracts::ResourceId{2}, {90, 0, 4, 4}});
    std::vector<std::uint32_t> pixels(120 * 80);
    assert(renderer.Render(*list, pixels.data(), 120, 80, 120 * 4));
    // BGRA little-endian: opaque background and red card must both appear.
    assert(pixels[0] == 0xFF102030);
    bool red = false;
    for (auto pixel : pixels) {
        if (pixel == 0xFFFF0000) {
            red = true;
        }
    }
    assert(red);
    const auto card = scene.Bounds({2, 1});
    assert(pixels[static_cast<int>(card.y) * 120] ==
           0xFF102030);               // rounded corner remains background
    assert(pixels[90] == 0xFFFF0000); // PNG resource reached Skia drawImageRect
    auto alternate = *list;
    for (auto &command : alternate.commands) {
        if (auto *run = std::get_if<prism::contracts::DrawGlyphRun>(&command)) {
            run->font = alternate_font;
        }
    }
    assert(renderer.Render(alternate, pixels.data(), 120, 80, 120 * 4));
    auto missing_font = alternate;
    for (auto &command : missing_font.commands) {
        if (auto *run = std::get_if<prism::contracts::DrawGlyphRun>(&command)) {
            run->font = prism::contracts::ResourceId{999};
        }
    }
    assert(!renderer.Render(missing_font, pixels.data(), 120, 80, 120 * 4));
    auto missing_image = *list;
    for (auto &command : missing_image.commands) {
        if (auto *image_command = std::get_if<prism::contracts::DrawImage>(&command)) {
            image_command->image = prism::contracts::ResourceId{998};
        }
    }
    assert(!renderer.Render(missing_image, pixels.data(), 120, 80, 120 * 4));
    auto invalid = *list;
    invalid.commands.push_back(prism::contracts::PopClip{});
    assert(!renderer.Render(invalid, pixels.data(), 120, 80, 120 * 4));
    assert(!renderer.Render(*list, pixels.data(), 0, 80, 120 * 4));
    // Transparent surfaces use premultiplied alpha, including diagnostic CPU
    // targets. This is observable at rounded corners and below the glass tint.
    prism::contracts::DisplayList glass;
    glass.window = {1};
    glass.commands.emplace_back(prism::contracts::FillRect{{10, 10, 20, 20}, {255, 0, 0, 128}});
    assert(renderer.Render(glass, pixels.data(), 120, 80, 120 * 4));
    assert(pixels[0] == 0 && pixels[15 * 120 + 15] == 0x80800000);
    glass.commands.clear();
    glass.commands.emplace_back(prism::contracts::PushClipRoundedRect{{10, 10, 20, 20}, 8});
    glass.commands.emplace_back(prism::contracts::FillRect{{0, 0, 120, 80}, {255, 255, 255, 255}});
    glass.commands.emplace_back(prism::contracts::PopClip{});
    assert(renderer.Render(glass, pixels.data(), 120, 80, 120 * 4));
    assert(pixels[10 * 120 + 10] == 0 && pixels[20 * 120 + 20] == 0xFFFFFFFF);
    glass.commands.clear();
    glass.commands.emplace_back(
        prism::contracts::RoundedRectShadow{{30, 30, 20, 20}, 5, 4, 3, {0, 0, 0, 128}, false});
    glass.commands.emplace_back(prism::contracts::DrawIcon{
        prism::contracts::VectorIcon::Music, {32, 32, 16, 16}, {255, 255, 255, 255}});
    assert(renderer.Render(glass, pixels.data(), 120, 80, 120 * 4));
    assert(pixels[0] == 0 && (pixels[52 * 120 + 40] >> 24) > 0);
    bool vector_white = false;
    for (auto pixel : pixels) {
        if ((pixel & 0xFFFFFF) == 0xFFFFFF) {
            vector_white = true;
        }
    }
    assert(vector_white);
    glass.commands.clear();
    glass.commands.emplace_back(
        prism::contracts::DrawImage{{2}, {10, 10, 20, 10}, prism::contracts::ImageFit::Contain});
    assert(renderer.Render(glass, pixels.data(), 120, 80, 120 * 4));
    assert(pixels[15 * 120 + 10] == 0 && (pixels[15 * 120 + 16] >> 24) == 255);
}
