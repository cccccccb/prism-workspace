#include "prism/contracts/rounded_region.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"
#include <cassert>
using namespace prism;

namespace {
runtime::ShapedText Shape(std::string_view text, double font)
{
    return {{}, text.size() * font / 2, font};
}

void Check(double radius)
{
    runtime::Scene scene(
        runtime::ParseBlueprint(
            "Card { InteractionTarget(action: \"panel\", width: 41, height: 33, inset: 0.25, "
            "background: #FFFFFFFF, backdropBlur: 12, cornerRadius: " +
            std::to_string(radius) + ") }"),
        Shape);
    scene.SetViewport({64, 64});
    auto list = scene.Build({1});
    assert(list);
    const auto effects = scene.SurfaceEffects();
    assert(effects.size() == 1);
    const auto effect = effects.front();
    assert(effect.corner_radius == std::min(radius, 16.5));
    const auto regions = scene.InputRegions();
    assert(regions.size() == 1);
    assert(regions[0].bounds == effect.bounds && regions[0].corner_radius == effect.corner_radius);
    const auto mask = contracts::RasterizeRoundedIntersection(regions);
    const auto snapshot = scene.InputGeometry();
    render_skia::RasterRenderer renderer("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    std::vector<std::uint32_t> pixels(64 * 64);
    assert(renderer.Render(*list, pixels.data(), 64, 64, 256));
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const contracts::LogicalPoint point{x + .5, y + .5};
            const bool hit = contracts::RoundedRegionContains(point, regions[0]);
            assert(bool(scene.HitTest(point)) == hit);
            assert(bool(scene.HitTest(point, *snapshot)) == hit);
            bool accepted = false;
            for (auto rect : mask) {
                accepted = accepted || (point.x >= rect.x && point.x < rect.x + rect.width &&
                                        point.y >= rect.y && point.y < rect.y + rect.height);
            }
            assert(accepted == hit);
            const auto alpha = pixels[y * 64 + x] >> 24;
            // AA edge coverage is continuous; compare binary geometry only where
            // Skia reports an unambiguous fully covered or fully empty pixel.
            if (alpha == 255) {
                assert(hit);
            }
            if (alpha == 0) {
                assert(!hit);
            }
        }
    }
}
} // namespace

int main()
{
    for (double radius : {0.0, 1.0, 8.0, 64.0}) {
        Check(radius);
    }
}
