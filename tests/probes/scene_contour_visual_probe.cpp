#include "prism/contracts/contour.hpp"
#include "prism/contracts/display_list_validation.hpp"
#include "prism/contracts/rounded_region.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/runtime/text_shaper.hpp"
#include "prism/theme/compiler.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace prism;

namespace {
constexpr int width = 240;
constexpr int height = 224;

std::string ReadRecipe(const std::filesystem::path &root)
{
    std::ifstream input(root / "docs/SURFACE_CONTOUR_CONTRACT.md");
    assert(input.good());
    const std::string document{std::istreambuf_iterator<char>(input), {}};
    const auto heading = document.find("### 静态连接颈配方");
    assert(heading != std::string::npos);
    constexpr std::string_view opening = "\n```prism\n";
    const auto fence = document.find(opening, heading);
    assert(fence != std::string::npos);
    const auto start = fence + opening.size();
    const auto end = document.find("\n```", start);
    assert(end != std::string::npos);
    const auto recipe = document.substr(start, end - start);
    assert(recipe.find("Cubic(") != std::string::npos);
    return recipe;
}

const runtime::InputSnapshotNode &FindPanel(const runtime::InputSnapshot &snapshot)
{
    const runtime::InputSnapshotNode *panel = nullptr;
    for (const auto &node : snapshot.nodes) {
        if (node.contour) {
            assert(!panel);
            panel = &node;
        }
    }
    assert(panel && panel->visible && panel->enabled && panel->clip);
    assert(!panel->interactive && panel->radius == 0);
    assert(panel->bounds == contracts::LogicalRect(24, 64, 160, 104));
    assert(!snapshot.Find(snapshot.root)->contour);
    return *panel;
}

void CheckShape(const contracts::Contour &contour)
{
    contracts::ValidateContour(contour);
    // The sixteen authored segments include cubics; preparation must retain
    // their intermediate samples rather than reducing them to endpoint lines.
    assert(contour.points.size() > 16);
    assert(contracts::ContourBounds(contour) == contracts::LogicalRect(24, 64, 160, 104));
    assert(contracts::ContourContains(contour, {104, 68}));
    assert(contracts::ContourContains(contour, {104, 104}));
    assert(!contracts::ContourContains(contour, {34, 72}));
}

contracts::Color ColorToken(const contracts::ThemeSnapshot &theme, std::string_view name)
{
    const auto color = contracts::ThemeColorValue(theme, name);
    assert(color);
    return *color;
}

void CheckCommands(const contracts::DisplayList &list, const contracts::Contour &contour,
                   const contracts::ThemeSnapshot &theme, contracts::ResourceId font)
{
    contracts::ValidateDisplayList(list);
    int fills = 0;
    int strokes = 0;
    int outer_shadows = 0;
    int inner_shadows = 0;
    int clips = 0;
    int glyph_runs = 0;
    int volume_icons = 0;
    for (const auto &command : list.commands) {
        if (const auto *fill = std::get_if<contracts::FillContour>(&command)) {
            ++fills;
            assert(fill->contour == contour);
            assert(fill->color == ColorToken(theme, "controlSecondary"));
        } else if (const auto *stroke = std::get_if<contracts::StrokeContour>(&command)) {
            ++strokes;
            assert(stroke->contour == contour && stroke->width == 1);
            assert(stroke->color == ColorToken(theme, "controlOutline"));
        } else if (const auto *shadow = std::get_if<contracts::ContourShadow>(&command)) {
            assert(shadow->contour == contour);
            if (shadow->inset) {
                ++inner_shadows;
                assert(shadow->blur == 1 && shadow->offset_y == 1);
                assert(shadow->color == contracts::Color(255, 255, 255, 26));
            } else {
                ++outer_shadows;
                assert(shadow->blur == 12 && shadow->offset_y == 4);
                assert(shadow->color == contracts::Color(0, 0, 0, 64));
            }
        } else if (const auto *clip = std::get_if<contracts::PushClipContour>(&command)) {
            ++clips;
            assert(clip->contour == contour);
        } else if (const auto *glyphs = std::get_if<contracts::DrawGlyphRun>(&command)) {
            ++glyph_runs;
            assert(glyphs->font == font && !glyphs->glyphs.empty());
        } else if (const auto *icon = std::get_if<contracts::DrawIcon>(&command)) {
            if (icon->icon == contracts::VectorIcon::Volume) {
                ++volume_icons;
            }
        }
    }
    assert(fills == 1 && strokes == 1 && outer_shadows == 1 && inner_shadows == 1);
    assert(clips == 1 && glyph_runs == 3 && volume_icons == 1);
}

std::size_t CheckEffects(const runtime::Scene &scene, const contracts::Contour &contour)
{
    const auto effects = scene.SurfaceEffects();
    int contours = 0;
    for (const auto &effect : effects) {
        contracts::ValidateSurfaceEffectRegion(effect);
        if (effect.contour) {
            ++contours;
            assert(*effect.contour == contour &&
                   effect.bounds == contracts::ContourBounds(contour));
            assert(effect.corner_radius == 0 && effect.blur_radius == 12);
        }
    }
    // The window material may contribute another rounded effect.
    assert(contours == 1);
    return effects.size();
}

std::size_t CheckInput(const runtime::Scene &scene, const contracts::Contour &contour)
{
    const auto &regions = scene.InputRegions();
    assert(!regions.empty() && regions.size() <= 65536);
    for (const auto &region : regions) {
        assert(contracts::ValidRoundedRegion(region));
        assert(contracts::NormalizeRoundedRegion(region) == region);
        assert(region.bounds.width > 0 && region.bounds.height > 0);
        assert(region.bounds.x >= 0 && region.bounds.y >= 0);
        assert(region.bounds.x + region.bounds.width <= width);
        assert(region.bounds.y + region.bounds.height <= height);
    }
    const auto mask = contracts::RasterizeContourIntersection(
        std::span<const contracts::Contour>(&contour, 1), {0, 0, width, height});
    assert(!mask.empty());
    for (const auto &bounds : mask) {
        const contracts::SurfaceInputRegion rectangle{bounds, 0};
        assert(std::find(regions.begin(), regions.end(), rectangle) != regions.end());
    }
    return regions.size();
}

std::array<unsigned, 3> Background(int x, int y)
{
    return ((x / 24 + y / 24) % 2) ? std::array<unsigned, 3>{76, 83, 92}
                                   : std::array<unsigned, 3>{70, 77, 86};
}

char CompositeChannel(std::uint32_t pixel, unsigned shift, unsigned background)
{
    const auto alpha = pixel >> 24;
    const auto foreground = (pixel >> shift) & 255U;
    const auto channel = foreground + (background * (255U - alpha) + 127U) / 255U;
    return static_cast<char>(std::min(channel, 255U));
}

void WritePreview(const std::filesystem::path &directory, std::string_view material,
                  std::string_view scheme, const std::vector<std::uint32_t> &pixels)
{
    std::filesystem::create_directories(directory);
    const auto name = "scene-contour-" + std::string(material) + "-" + std::string(scheme) + ".ppm";
    std::ofstream output(directory / name, std::ios::binary);
    assert(output.good());
    output << "P6\n" << width << ' ' << height << "\n255\n";
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto pixel = pixels[y * width + x];
            const auto background = Background(x, y);
            const char rgb[]{CompositeChannel(pixel, 16, background[0]),
                             CompositeChannel(pixel, 8, background[1]),
                             CompositeChannel(pixel, 0, background[2])};
            output.write(rgb, 3);
        }
    }
    assert(output.good());
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    const std::filesystem::path root(argv[1]);
    const auto source = ReadRecipe(root);
    constexpr auto font = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    runtime::TextShaper shaper(font);
    render_skia::RasterRenderer renderer(font);
    assert(shaper.Ready() && renderer.Ready());
    const auto shape = std::bind_front(
        static_cast<runtime::ShapedText (runtime::TextShaper::*)(std::string_view, double)>(
            &runtime::TextShaper::Shape),
        &shaper);
    std::optional<contracts::Contour> reference;

    for (auto material : {"glass", "translucent", "transparent", "square"}) {
        for (auto scheme : {"light", "dark"}) {
            const auto current_theme =
                theme::LoadTheme(root / "resources/themes", material, 1, scheme);
            runtime::Scene scene(runtime::ParseBlueprint(source), shape, shaper.FontId(),
                                 current_theme);
            assert(scene.SetViewport({width, height}));
            const auto list = scene.Build({1});
            assert(list && scene.InputGeometry());
            const auto snapshot = scene.InputGeometry();
            const auto &panel = FindPanel(*snapshot);
            CheckShape(*panel.contour);
            CheckCommands(*list, *panel.contour, current_theme, shaper.FontId());
            const auto effects = CheckEffects(scene, *panel.contour);
            const auto input_regions = CheckInput(scene, *panel.contour);
            if (reference) {
                assert(*reference == *panel.contour);
            } else {
                reference = *panel.contour;
            }

            std::vector<std::uint32_t> pixels(width * height);
            assert(renderer.Render(*list, pixels.data(), width, height, width * 4));
            if (argc == 3) {
                WritePreview(argv[2], material, scheme, pixels);
            }
            std::cout << material << ' ' << scheme << ": vertices=" << panel.contour->points.size()
                      << ", effects=" << effects << ", input_regions=" << input_regions << '\n';
        }
    }
}
