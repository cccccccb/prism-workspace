#include "prism/contracts/display_list_validation.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/task_paint.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

using namespace prism;

namespace {
constexpr int width = 256;
constexpr int height = 160;
using Pixels = std::vector<std::uint8_t>;

runtime::TaskPaintSource Source()
{
    return {{{{1}, {2}}, {3, 4}, 5}, 6, 7, 1, {width, height}, 1, 8, 9};
}

contracts::DisplayList Body(contracts::Color content)
{
    return {{1},
            1,
            {contracts::FillRect{{0, 0, width, height}, {32, 38, 44, 255}},
             contracts::FillRoundedRect{{12, 20, 60, 28}, 4, content}}};
}

runtime::TaskPaintFragment Paint()
{
    return {Source(),
            {contracts::PushTransform{{1, 0, 0, 0, 1, 0}}, contracts::PushOpacity{.85},
             contracts::RoundedRectShadow{{145, 52, 64, 48}, 9, 8, 4, {0, 0, 0, 150}, false},
             contracts::FillRoundedRect{{145, 52, 64, 48}, 9, {70, 95, 130, 240}},
             contracts::StrokeRoundedRect{{145, 52, 64, 48}, 9, 2, {160, 190, 220, 180}},
             contracts::RoundedRectShadow{{145, 52, 64, 48}, 9, 2, 1, {0, 0, 0, 80}, true},
             contracts::PopOpacity{}, contracts::PopTransform{}}};
}

bool Covers(const contracts::DamageRegion &damage, int x, int y)
{
    if (damage.full) {
        return true;
    }
    for (const auto &rect : damage.rects) {
        if (x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height) {
            return true;
        }
    }
    return false;
}

void CheckReplay(render_skia::RasterRenderer &renderer, const contracts::DisplayList &previous,
                 const contracts::DisplayList &next, bool expect_full)
{
    contracts::ValidateDisplayList(previous);
    contracts::ValidateDisplayList(next);
    const auto damage =
        renderer.CompareDamage(&previous, next, width, height, renderer.ResourceEpoch());
    assert(damage.full == expect_full);
    assert(damage.full || !damage.rects.empty());

    Pixels before(width * height * 4);
    Pixels expected(before.size());
    assert(renderer.Render(previous, before.data(), width, height, width * 4));
    assert(renderer.Render(next, expected.data(), width, height, width * 4));
    auto repaired = before;
    assert(renderer.Render(next, repaired.data(), width, height, width * 4, damage));
    assert(repaired == expected);

    std::size_t changed_outside_panel = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto offset = static_cast<std::size_t>((y * width + x) * 4);
            bool changed = false;
            for (int channel = 0; channel < 4; ++channel) {
                changed = changed || before[offset + channel] != expected[offset + channel];
            }
            if (changed) {
                assert(Covers(damage, x, y));
                if (x < 145 || x >= 209 || y < 52 || y >= 100) {
                    ++changed_outside_panel;
                }
            }
        }
    }
    assert(changed_outside_panel > 0);
}

void CheckEnvironmentFences()
{
    const auto source = Source();
    auto current = source;
    ++current.projection;
    ++current.frame_sequence;
    assert(runtime::MatchesTaskPaintEnvironment(source, current));
    ++current.identity.cycle;
    assert(!runtime::MatchesTaskPaintEnvironment(source, current));
    current = source;
    ++current.resource_epoch;
    assert(!runtime::MatchesTaskPaintEnvironment(source, current));
    current = source;
    ++current.theme_generation;
    assert(!runtime::MatchesTaskPaintEnvironment(source, current));
    current = source;
    ++current.configure_count;
    assert(!runtime::MatchesTaskPaintEnvironment(source, current));
    current = source;
    ++current.buffer_size.width;
    assert(!runtime::MatchesTaskPaintEnvironment(source, current));
    current = source;
    current.scale = std::numeric_limits<double>::quiet_NaN();
    assert(!runtime::MatchesTaskPaintEnvironment(source, current));
    current = source;
    current.identity = {};
    assert(!runtime::MatchesTaskPaintEnvironment(current, current));
}
} // namespace

int main()
{
    CheckEnvironmentFences();
    std::cout << "task_paint_composition_test: environment fences passed\n";

    render_skia::RasterRenderer renderer("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    assert(renderer.Ready());
    const auto paint = Paint();
    const auto saved_paint = paint.commands;
    const auto old_body = Body({160, 170, 180, 255});
    auto body = Body({190, 80, 65, 255});
    const auto saved_body = body.commands;
    const auto previous = runtime::ComposeTaskPaint(old_body, paint);
    const auto next = runtime::ComposeTaskPaint(body, paint);
    assert(paint.commands == saved_paint && body.commands == saved_body);
    body.commands.clear();
    assert(next.commands.size() == saved_body.size() + saved_paint.size());
    CheckReplay(renderer, previous, next, false);
    std::cout << "task_paint_composition_test: current body and detached paint replay passed\n";

    auto moved = paint;
    std::get<contracts::PushTransform>(moved.commands.front()).values[2] = -55;
    std::get<contracts::PushTransform>(moved.commands.front()).values[5] = 26;
    const auto moved_list = runtime::ComposeTaskPaint(Body({190, 80, 65, 255}), moved);
    CheckReplay(renderer, next, moved_list, false);
    std::cout << "task_paint_composition_test: old/new transformed shadow damage passed\n";

    CheckReplay(renderer, moved_list, Body({190, 80, 65, 255}), true);
    std::cout << "task_paint_composition_test: removal repair passed\n";
}
