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
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace prism;

namespace {
struct Case {
    std::string_view name;
    int width;
    int height;
    double lead;
    double left;
    bool above;
    bool attached;
};

constexpr std::array cases{
    Case{"below", 480, 480, 12, 140, false, true}, Case{"above", 480, 480, 300, 140, true, true},
    Case{"right", 480, 480, 12, 350, false, true}, Case{"edge", 150, 480, 12, 1, false, false}};

std::string ReadFixture(const std::filesystem::path &root)
{
    std::ifstream input(root / "tests/fixtures/interface-system/attached-panel.prism");
    assert(input.good());
    return {std::istreambuf_iterator<char>(input), {}};
}

contracts::NodeId FindAction(const runtime::InputSnapshot &snapshot, std::string_view action)
{
    for (const auto &node : snapshot.nodes) {
        if (node.id && node.action == action) {
            return node.id;
        }
    }
    assert(false);
    return {};
}

const runtime::InputSnapshotNode &Panel(const runtime::InputSnapshot &snapshot)
{
    const runtime::InputSnapshotNode *panel = nullptr;
    for (const auto &node : snapshot.nodes) {
        if (node.visible && node.contour) {
            assert(!panel);
            panel = &node;
        }
    }
    assert(panel && panel->clip && panel->interactive && panel->radius == 0);
    return *panel;
}

void CheckPlacement(const runtime::Scene &scene, const runtime::InputSnapshotNode &panel,
                    contracts::NodeId anchor_id, const Case &test,
                    const contracts::ThemeSnapshot &theme)
{
    contracts::ValidateContour(*panel.contour);
    const auto extent = contracts::ContourBounds(*panel.contour);
    const auto anchor = scene.Bounds(anchor_id);
    const auto neck_height = contracts::ThemeNumberValue(theme, "space_sm");
    assert(neck_height);
    assert(panel.bounds.x >= 8 && panel.bounds.x + panel.bounds.width <= test.width - 8);
    if (!test.attached) {
        assert(extent == panel.bounds);
        assert(panel.bounds.width == test.width - 16);
    } else {
        assert(extent.height == panel.bounds.height + *neck_height);
        if (test.above) {
            assert(extent.y == panel.bounds.y);
            assert(extent.y + extent.height == anchor.y - 8);
        } else {
            assert(extent.y == anchor.y + anchor.height + 8);
            assert(panel.bounds.y == extent.y + *neck_height);
        }
    }
    if (theme.id == "square") {
        assert(panel.contour->points.size() <= 12);
    } else {
        assert(panel.contour->points.size() > 12);
    }
    if (test.attached) {
        const double y = test.above ? panel.bounds.y + panel.bounds.height + *neck_height / 2
                                    : panel.bounds.y - *neck_height / 2;
        double first = -1;
        double last = -1;
        for (int x = static_cast<int>(extent.x); x < extent.x + extent.width; ++x) {
            if (contracts::ContourContains(*panel.contour, {x + .5, y})) {
                if (first < 0) {
                    first = x + .5;
                }
                last = x + .5;
            }
        }
        assert(first >= 0);
        const contracts::LogicalPoint neck{(first + last) / 2, y};
        assert(scene.HitTest(neck)->node == panel.id);
        assert(scene.HitTest(neck, *scene.InputGeometry())->node == panel.id);
        const contracts::LogicalPoint wing{panel.bounds.x + 1, y};
        assert(!contracts::ContourContains(*panel.contour, wing));
        const auto live_wing = scene.HitTest(wing);
        const auto frozen_wing = scene.HitTest(wing, *scene.InputGeometry());
        assert(!live_wing || live_wing->node != panel.id);
        assert(!frozen_wing || frozen_wing->node != panel.id);
    }
    const auto volume = scene.Bounds(FindAction(*scene.InputGeometry(), "volume"));
    assert(volume.width > 0 && volume.height == 32);
    assert(volume.x >= panel.bounds.x + 16);
    assert(volume.y >= panel.bounds.y + 16);
    assert(volume.x + volume.width <= panel.bounds.x + panel.bounds.width - 16);
    assert(volume.y + volume.height <= panel.bounds.y + panel.bounds.height - 16);
}

void CheckCommands(const contracts::DisplayList &list, const contracts::Contour &contour,
                   const contracts::ThemeMaterial &material, contracts::ResourceId font)
{
    contracts::ValidateDisplayList(list);
    int fills = 0;
    int strokes = 0;
    int clips = 0;
    int glyphs = 0;
    int volume_icons = 0;
    for (const auto &command : list.commands) {
        if (const auto *fill = std::get_if<contracts::FillContour>(&command)) {
            ++fills;
            assert(fill->contour == contour && fill->color == material.tint);
        } else if (const auto *stroke = std::get_if<contracts::StrokeContour>(&command)) {
            ++strokes;
            assert(stroke->contour == contour);
            assert(stroke->width == material.border_width && stroke->color == material.border);
        } else if (const auto *shadow = std::get_if<contracts::ContourShadow>(&command)) {
            assert(shadow->contour == contour);
            assert(shadow->blur ==
                   (shadow->inset ? material.inner_shadow_blur : material.shadow_blur));
            assert(shadow->offset_y ==
                   (shadow->inset ? material.inner_shadow_y : material.shadow_y));
        } else if (const auto *clip = std::get_if<contracts::PushClipContour>(&command)) {
            ++clips;
            assert(clip->contour == contour);
        } else if (const auto *run = std::get_if<contracts::DrawGlyphRun>(&command)) {
            ++glyphs;
            assert(run->font == font && !run->glyphs.empty());
        } else if (const auto *icon = std::get_if<contracts::DrawIcon>(&command)) {
            volume_icons += icon->icon == contracts::VectorIcon::Volume;
        }
    }
    assert(fills == (material.tint.a ? 1 : 0));
    assert(strokes == (material.border_width > 0 && material.border.a ? 1 : 0));
    assert(clips == 1 && glyphs >= 7 && volume_icons == 1);
}

std::size_t CheckRegions(const runtime::Scene &scene, const contracts::Contour &contour,
                         const contracts::ThemeMaterial &material, const Case &test)
{
    const auto effects = scene.SurfaceEffects();
    int contour_effects = 0;
    for (const auto &effect : effects) {
        contracts::ValidateSurfaceEffectRegion(effect);
        if (effect.contour) {
            ++contour_effects;
            assert(*effect.contour == contour &&
                   effect.bounds == contracts::ContourBounds(contour));
            assert(effect.corner_radius == 0 && effect.blur_radius == material.backdrop_blur);
        }
    }
    assert(contour_effects == (material.backdrop_blur > 0 ? 1 : 0));

    const auto &regions = scene.InputRegions();
    assert(!regions.empty() && regions.size() <= 65536);
    for (const auto &region : regions) {
        assert(contracts::ValidRoundedRegion(region));
        assert(contracts::NormalizeRoundedRegion(region) == region);
        assert(region.bounds.width > 0 && region.bounds.height > 0);
        assert(region.bounds.x >= 0 && region.bounds.y >= 0);
        assert(region.bounds.x + region.bounds.width <= test.width);
        assert(region.bounds.y + region.bounds.height <= test.height);
    }
    const auto mask =
        contracts::RasterizeContourIntersection(std::span<const contracts::Contour>(&contour, 1),
                                                {0, 0, double(test.width), double(test.height)});
    for (const auto &bounds : mask) {
        // The owner barrier may represent this mask with larger rectangles.
        for (int y = static_cast<int>(bounds.y); y < bounds.y + bounds.height; ++y) {
            for (int x = static_cast<int>(bounds.x); x < bounds.x + bounds.width; ++x) {
                const contracts::LogicalPoint point{x + .5, y + .5};
                const bool covered =
                    std::any_of(regions.begin(), regions.end(), [point](const auto &region) {
                        return contracts::RoundedRegionContains(point, region);
                    });
                assert(covered);
            }
        }
    }
    return regions.size();
}

char Channel(std::uint32_t pixel, unsigned shift, unsigned background)
{
    const auto alpha = pixel >> 24;
    const auto channel = ((pixel >> shift) & 255U) + (background * (255U - alpha) + 127U) / 255U;
    return static_cast<char>(std::min(channel, 255U));
}

void WritePreview(const std::filesystem::path &directory, std::string_view material,
                  std::string_view scheme, const Case &test, std::string_view state,
                  const std::vector<std::uint32_t> &pixels)
{
    std::filesystem::create_directories(directory);
    const auto name = "attached-panel-" + std::string(material) + "-" + std::string(scheme) + "-" +
                      std::string(test.name) + "-" + std::string(state) + ".ppm";
    std::ofstream output(directory / name, std::ios::binary);
    assert(output.good());
    output << "P6\n" << test.width << ' ' << test.height << "\n255\n";
    for (int y = 0; y < test.height; ++y) {
        for (int x = 0; x < test.width; ++x) {
            const auto pixel = pixels[y * test.width + x];
            const unsigned background = ((x / 24 + y / 24) % 2) ? 83 : 76;
            const char rgb[]{Channel(pixel, 16, background), Channel(pixel, 8, background),
                             Channel(pixel, 0, background)};
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
    const auto source = ReadFixture(root);
    constexpr auto font = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    runtime::TextShaper shaper(font);
    render_skia::RasterRenderer renderer(font);
    assert(shaper.Ready() && renderer.Ready());
    const auto shape = std::bind_front(
        static_cast<runtime::ShapedText (runtime::TextShaper::*)(std::string_view, double)>(
            &runtime::TextShaper::Shape),
        &shaper);

    for (auto material_name : {"glass", "translucent", "transparent", "square"}) {
        for (auto scheme : {"light", "dark"}) {
            const auto theme =
                theme::LoadTheme(root / "resources/themes", material_name, 1, scheme);
            const auto *material = contracts::FindThemeMaterial(theme, "panel");
            assert(material);
            for (const auto &test : cases) {
                runtime::Scene scene(runtime::ParseBlueprint(source), shape, shaper.FontId(),
                                     theme);
                assert(scene.SetBinding("anchorLead", test.lead));
                assert(scene.SetBinding("anchorLeft", test.left));
                scene.SetBinding("muted", false);
                assert(scene.SetViewport({double(test.width), double(test.height)}));
                const auto closed = scene.Build({1});
                assert(closed && scene.InputGeometry());
                scene.AcknowledgeComposite();
                const auto anchor = FindAction(*scene.InputGeometry(), "sound");
                std::vector<std::uint32_t> before(test.width * test.height);
                assert(renderer.Render(*closed, before.data(), test.width, test.height,
                                       test.width * 4));

                assert(scene.OpenPopup(anchor));
                const auto opened = scene.Build({1});
                assert(opened && scene.InputGeometry());
                scene.AcknowledgeComposite();
                const auto snapshot = scene.InputGeometry();
                const auto &panel = Panel(*snapshot);
                CheckPlacement(scene, panel, anchor, test, theme);
                CheckCommands(*opened, *panel.contour, *material, shaper.FontId());
                const auto input_regions = CheckRegions(scene, *panel.contour, *material, test);
                assert(FindAction(*snapshot, "output") && FindAction(*snapshot, "mute"));
                std::vector<std::uint32_t> after(test.width * test.height);
                assert(renderer.Render(*opened, after.data(), test.width, test.height,
                                       test.width * 4));
                assert(before != after);
                if (argc == 3) {
                    WritePreview(argv[2], material_name, scheme, test, "closed", before);
                    WritePreview(argv[2], material_name, scheme, test, "open", after);
                }
                std::cout << material_name << ' ' << scheme << ' ' << test.name
                          << ": vertices=" << panel.contour->points.size()
                          << ", input_regions=" << input_regions << '\n';
            }
        }
    }
}
