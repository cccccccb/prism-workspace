#include "prism/contracts/display_list_validation.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace {
using namespace prism::contracts;
using prism::render_skia::RasterRenderer;
constexpr int width = 320;
constexpr int height = 192;

Contour Rectangle(double x, double y, double w, double h)
{
    return {{{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}};
}

Contour ConnectedPanel()
{
    // One contour joins the neck and rounded panel. There is no overlapping
    // second shape, internal border or duplicate source-over at the junction.
    return PrepareContour(
        {{116, 30},
         {ContourCubic{{122, 30}, {122, 14}, {128, 14}},
          ContourCubic{{134, 14}, {134, 30}, {140, 30}}, ContourLine{{180, 30}},
          ContourCubic{{186.628, 30}, {192, 35.372}, {192, 42}}, ContourLine{{192, 128}},
          ContourCubic{{192, 134.628}, {186.628, 140}, {180, 140}}, ContourLine{{76, 140}},
          ContourCubic{{69.372, 140}, {64, 134.628}, {64, 128}}, ContourLine{{64, 42}},
          ContourCubic{{64, 35.372}, {69.372, 30}, {76, 30}}, ContourLine{{116, 30}}}});
}

std::vector<std::uint32_t> Render(RasterRenderer &renderer, std::vector<DrawCommand> commands)
{
    DisplayList list{{1}, 1, std::move(commands)};
    std::vector<std::uint32_t> pixels(width * height);
    assert(renderer.Render(list, pixels.data(), width, height, width * 4));
    return pixels;
}

unsigned Alpha(const std::vector<std::uint32_t> &pixels, int x, int y)
{
    return pixels[y * width + x] >> 24;
}

void FillAndClip(RasterRenderer &renderer, const Contour &panel)
{
    const auto fill = Render(renderer, {FillContour{panel, {255, 255, 255, 128}}});
    for (int y = 24; y <= 38; ++y) {
        // This passes through the neck-to-body join. Layering two translucent
        // shapes would produce alpha 192 rather than the requested 128.
        assert(Alpha(fill, 128, y) == 128);
    }
    assert(Alpha(fill, 128, 80) == 128);
    assert(Alpha(fill, 60, 80) == 0);
    assert(Alpha(fill, 100, 20) == 0);

    auto reversed = panel;
    std::reverse(reversed.points.begin(), reversed.points.end());
    const auto clockwise = Render(renderer, {FillContour{reversed, {255, 255, 255, 128}}});
    assert(clockwise == fill);
    const auto decoded = DecodeContour(EncodeContour(panel));
    assert(Render(renderer, {FillContour{decoded, {255, 255, 255, 128}}}) == fill);

    const auto clipped =
        Render(renderer, {PushClipContour{panel},
                          FillRect{{0, 0, width, height}, {255, 255, 255, 255}}, PopClip{}});
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool inside = ContourContains(panel, {x + .5, y + .5});
            const auto alpha = Alpha(clipped, x, y);
            // Skia AA is continuous; unambiguous pixels must agree with the
            // shared binary geometry used for input and effect masks.
            if (alpha == 255) {
                assert(inside);
            }
            if (alpha == 0) {
                assert(!inside);
            }
        }
    }

    const Contour concave{
        {{20, 20}, {60, 20}, {60, 80}, {48, 80}, {48, 32}, {32, 32}, {32, 80}, {20, 80}}};
    const auto recess =
        Render(renderer, {PushClipContour{concave},
                          FillRect{{0, 0, width, height}, {255, 255, 255, 255}}, PopClip{}});
    assert(Alpha(recess, 24, 60) == 255);
    assert(Alpha(recess, 54, 60) == 255);
    assert(Alpha(recess, 40, 60) == 0);
}

void StrokeAndShadow(RasterRenderer &renderer, const Contour &panel)
{
    const auto border = Render(renderer, {StrokeContour{panel, 3, {255, 255, 255, 255}}});
    assert(Alpha(border, 64, 80) == 255);
    assert(Alpha(border, 66, 80) == 255);
    assert(Alpha(border, 68, 80) == 0);
    assert(Alpha(border, 63, 80) == 0);
    assert(Alpha(border, 128, 30) == 0);
    assert(Alpha(border, 128, 80) == 0);
    const auto no_border = Render(renderer, {StrokeContour{panel, 0, {255, 255, 255, 255}}});
    assert(std::all_of(no_border.begin(), no_border.end(), [](auto pixel) { return pixel == 0; }));

    const auto outer = Render(renderer, {ContourShadow{panel, 4, 5, {0, 0, 0, 180}, false}});
    assert(Alpha(outer, 128, 146) > 0);
    assert(Alpha(outer, 58, 80) > 0);
    assert(Alpha(outer, 128, 80) == 180);
    const auto inset = Render(renderer, {ContourShadow{panel, 4, 5, {0, 0, 0, 180}, true}});
    assert(Alpha(inset, 80, 31) > 0);
    assert(Alpha(inset, 64, 80) > 0);
    assert(Alpha(inset, 60, 80) == 0);
    assert(Alpha(inset, 128, 145) == 0);
    assert(Alpha(inset, 128, 80) == 0);

    const auto rect = Rectangle(16, 16, 32, 32);
    const auto shifted = Render(renderer, {ContourShadow{rect, 0, -4, {0, 0, 0, 255}, true}});
    assert(Alpha(shifted, 32, 46) == 255);
    assert(Alpha(shifted, 32, 32) == 0);
    assert(Alpha(shifted, 32, 49) == 0);
}

bool InRepair(int x, int y, const DamageRegion &repair)
{
    if (repair.full) {
        return true;
    }
    for (const auto rect : repair.rects) {
        if (x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height) {
            return true;
        }
    }
    return false;
}

DisplayList DamageList(const Contour &panel)
{
    return {{1},
            1,
            {FillRect{{0, 0, width, height}, {19, 28, 43, 255}},
             PushTransform{{1.05, 0, 4.25, 0, .95, 7.5}}, PushOpacity{.65},
             ContourShadow{panel, 4, 5, {0, 0, 0, 130}, false},
             FillContour{panel, {73, 111, 152, 210}},
             StrokeContour{panel, 1.5, {180, 211, 239, 200}}, PushClipContour{panel},
             PushOpacity{.55}, FillRect{{120, 17, 28, 44}, {239, 165, 94, 240}},
             FillRect{{130, 24, 24, 36}, {55, 196, 158, 220}}, PopOpacity{}, PopClip{},
             ContourShadow{panel, 2, 2, {14, 21, 35, 100}, true}, PopOpacity{}, PopTransform{}}};
}

void CompareRepair(RasterRenderer &renderer, const DisplayList &previous, const DisplayList &next,
                   bool expect_full = false, bool expect_empty = false)
{
    std::vector<std::uint32_t> before(width * height), expected(width * height);
    assert(renderer.Render(previous, before.data(), width, height, width * 4));
    assert(renderer.Render(next, expected.data(), width, height, width * 4));
    const auto repair =
        renderer.CompareDamage(&previous, next, width, height, renderer.ResourceEpoch());
    assert(repair.full == expect_full);
    assert((!repair.full && repair.rects.empty()) == expect_empty);

    auto actual = before;
    assert(renderer.Render(next, actual.data(), width, height, width * 4, repair));
    assert(actual == expected);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (!InRepair(x, y, repair)) {
                assert(before[y * width + x] == actual[y * width + x]);
            }
        }
    }
}

void Damage(RasterRenderer &renderer, const Contour &panel)
{
    auto previous = DamageList(panel);
    auto next = previous;
    ++next.generation;
    CompareRepair(renderer, previous, next, false, true);

    std::get<FillContour>(next.commands[4]).color = {143, 82, 176, 170};
    CompareRepair(renderer, previous, next);
    previous = next;
    for (auto &point : std::get<FillContour>(next.commands[4]).contour.points) {
        point.x += 6.25;
    }
    CompareRepair(renderer, previous, next);
    previous = next;
    auto &shadow = std::get<ContourShadow>(next.commands[3]);
    shadow.blur = 7;
    shadow.offset_y = -4;
    CompareRepair(renderer, previous, next);
    previous = next;
    std::get<StrokeContour>(next.commands[5]).width = 7.5;
    CompareRepair(renderer, previous, next);
    previous = next;
    std::get<ContourShadow>(next.commands[12]).blur = 6;
    CompareRepair(renderer, previous, next);
    previous = next;
    std::get<PushOpacity>(next.commands[2]).opacity = .37;
    CompareRepair(renderer, previous, next);
    previous = next;
    std::get<PushTransform>(next.commands[1]).values = {1.1, 0, 14.5, 0, .8, 18.25};
    CompareRepair(renderer, previous, next);
    previous = next;
    std::get<FillRect>(next.commands[8]).color = {217, 122, 74, 160};
    CompareRepair(renderer, previous, next);
    previous = next;
    std::get<PushOpacity>(next.commands[2]).opacity = 0;
    CompareRepair(renderer, previous, next);
    previous = next;
    std::get<FillContour>(next.commands[4]).color = {55, 171, 203, 255};
    CompareRepair(renderer, previous, next, false, true);
    previous = next;
    std::get<PushOpacity>(next.commands[2]).opacity = .9;
    CompareRepair(renderer, previous, next);
    previous = next;
    std::get<PushClipContour>(next.commands[6]).contour = Rectangle(70, 40, 110, 90);
    CompareRepair(renderer, previous, next, true);
}

template <typename Callable> void Rejected(Callable callable)
{
    bool rejected = false;
    try {
        callable();
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
}

void Validation(RasterRenderer &renderer, const Contour &panel)
{
    for (DrawCommand invalid :
         {DrawCommand{FillContour{Contour{}, {}}}, DrawCommand{StrokeContour{panel, -1, {}}},
          DrawCommand{StrokeContour{panel, std::numeric_limits<double>::quiet_NaN(), {}}},
          DrawCommand{ContourShadow{panel, -1, 0, {}, false}},
          DrawCommand{ContourShadow{panel, 1, std::numeric_limits<double>::infinity(), {}, false}},
          DrawCommand{PushClipContour{Contour{}}}}) {
        DisplayList list{{1}, 1, {invalid}};
        Rejected([&list] { ValidateDisplayList(list); });
        std::vector<std::uint32_t> pixels(width * height, 0x12345678);
        assert(!renderer.Render(list, pixels.data(), width, height, width * 4));
        assert(std::all_of(pixels.begin(), pixels.end(),
                           [](auto pixel) { return pixel == 0x12345678; }));
    }
    const DisplayList unclosed{{1}, 1, {PushClipContour{panel}}};
    Rejected([&unclosed] { ValidateDisplayList(unclosed); });
    const DisplayList crossed{
        {1}, 1, {PushClipContour{panel}, PushOpacity{.5}, PopClip{}, PopOpacity{}}};
    Rejected([&crossed] { ValidateDisplayList(crossed); });
}

void WriteEvidence(RasterRenderer &renderer, const Contour &panel, const std::filesystem::path &dir)
{
    std::filesystem::create_directories(dir);
    for (bool light : {false, true}) {
        const Color background = light ? Color{235, 241, 248, 255} : Color{12, 20, 32, 255};
        const Color material = light ? Color{245, 249, 253, 230} : Color{34, 52, 72, 230};
        const Color foreground = light ? Color{51, 72, 94, 230} : Color{220, 234, 249, 230};
        const auto pixels = Render(
            renderer,
            {FillRect{{0, 0, width, height}, background},
             ContourShadow{panel, 7, 6, {0, 0, 0, 75}, false}, FillContour{panel, material},
             StrokeContour{panel, 1, light ? Color{255, 255, 255, 230} : Color{136, 177, 217, 110}},
             ContourShadow{panel, 2, 2, {0, 0, 0, 35}, true},
             DrawIcon{VectorIcon::Volume, {80, 46, 20, 20}, foreground},
             FillRoundedRect{
                 {112, 54, 60, 4}, 2, light ? Color{177, 192, 209, 255} : Color{72, 92, 116, 255}},
             FillRoundedRect{{112, 54, 42, 4}, 2, {108, 166, 241, 255}},
             FillRect{{80, 80, 96, 1},
                      light ? Color{200, 211, 222, 180} : Color{88, 112, 138, 140}},
             DrawIcon{VectorIcon::Sun, {80, 100, 20, 20}, foreground},
             FillRoundedRect{{112, 103, 60, 14}, 7, {104, 165, 244, 230}},
             FillRoundedRect{{154, 105, 10, 10}, 5, {248, 250, 254, 255}}});
        std::ofstream output(dir / (light ? "connected-light.ppm" : "connected-dark.ppm"),
                             std::ios::binary);
        output << "P6\n" << width << ' ' << height << "\n255\n";
        for (const auto pixel : pixels) {
            const char rgb[]{char(pixel >> 16), char(pixel >> 8), char(pixel)};
            output.write(rgb, 3);
        }
        assert(output.good());
    }
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc <= 2);
    RasterRenderer renderer("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    assert(renderer.Ready());
    const auto panel = ConnectedPanel();
    FillAndClip(renderer, panel);
    StrokeAndShadow(renderer, panel);
    Damage(renderer, panel);
    Validation(renderer, panel);
    if (argc == 2) {
        WriteEvidence(renderer, panel, argv[1]);
    }
}
