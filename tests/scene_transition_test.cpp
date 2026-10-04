#include "prism/animation/timeline.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

using namespace prism;

namespace {
class FakeClock final : public animation::AnimationClock {
public:
    std::uint64_t now_ns{1'000'000'000};

    std::uint64_t NowNs() const noexcept override
    {
        return now_ns;
    }
};

runtime::ShapedText Shape(std::string_view text, double size)
{
    runtime::ShapedText shaped;
    shaped.width = static_cast<double>(text.size()) * size * 0.5;
    shaped.height = size;
    for (std::size_t index = 0; index < text.size(); ++index) {
        shaped.glyphs.push_back({static_cast<std::uint32_t>(text[index]),
                                 {static_cast<double>(index) * size * 0.5, size}});
    }
    return shaped;
}

contracts::DisplayList BuildAndCommit(runtime::Scene &scene)
{
    auto list = scene.Build({1});
    assert(list);
    scene.AcknowledgeComposite();
    return std::move(*list);
}

double ProgressWidth(const contracts::DisplayList &list, contracts::Color color)
{
    for (const auto &command : list.commands) {
        const auto *fill = std::get_if<contracts::FillRoundedRect>(&command);
        if (fill && fill->color == color) {
            return fill->bounds.width;
        }
    }
    assert(false);
    return 0;
}

contracts::Color GlyphColor(const contracts::DisplayList &list)
{
    for (const auto &command : list.commands) {
        const auto *run = std::get_if<contracts::DrawGlyphRun>(&command);
        if (run && !run->glyphs.empty()) {
            return run->color;
        }
    }
    assert(false);
    return {};
}

bool Near(double actual, double expected)
{
    return std::abs(actual - expected) < 0.001;
}

contracts::ThemeSnapshot Theme(std::uint64_t generation)
{
    contracts::ThemeSnapshot theme;
    theme.id = "fixture";
    theme.name = "Fixture";
    theme.generation = generation;
    theme.colors = {{"accent", {200, 117, 33, 255}}};
    return theme;
}

void CheckProgressAndRetarget()
{
    constexpr contracts::Color fill{200, 117, 33, 255};
    FakeClock clock;
    runtime::Scene scene(
        runtime::ParseBlueprint("Progress(value:$progress,width:100,height:10,foreground:#C87521FF)"
                                ".transition(property:\"value\",durationMs:200,easing:\"linear\")"),
        Shape);
    assert(scene.SetViewport({100, 10}));
    assert(scene.SetBinding("progress", 0.2));
    assert(Near(ProgressWidth(BuildAndCommit(scene), fill), 20));
    assert(scene.AnimationSample() == (runtime::AnimationSampleStamp{}));

    scene.EnableAnimations(&clock);
    const auto initial_pixels = scene.PixelsRevision();
    const auto initial_transaction = scene.TransactionRevision();
    assert(scene.SetBinding("progress", 0.8));
    assert(scene.TransactionRevision() == initial_transaction + 1);
    assert(scene.PixelsRevision() == initial_pixels);
    assert(scene.AnimationSample().revision == 0);
    assert(scene.HasActiveAnimations());
    assert(!scene.Build({1}));

    clock.now_ns += 50'000'000;
    assert(scene.AdvanceAnimations(clock.now_ns));
    assert(scene.PixelsRevision() > initial_pixels);
    const auto first_sample = scene.AnimationSample();
    assert(first_sample.revision == 1 && first_sample.time_ns == clock.now_ns);
    assert(Near(ProgressWidth(BuildAndCommit(scene), fill), 35));
    const auto layout_count = scene.GetRenderStats().layouts;

    // A new target starts at the current visual value, not the previous target.
    clock.now_ns += 50'000'000;
    assert(scene.SetBinding("progress", 0.1));
    const auto retarget_sample = scene.AnimationSample();
    assert(retarget_sample.revision == first_sample.revision + 1 &&
           retarget_sample.time_ns == clock.now_ns);
    assert(Near(ProgressWidth(BuildAndCommit(scene), fill), 50));
    assert(scene.GetRenderStats().layouts == layout_count);

    clock.now_ns += 50'000'000;
    assert(scene.AdvanceAnimations(clock.now_ns));
    assert(Near(ProgressWidth(BuildAndCommit(scene), fill), 40));

    clock.now_ns += 150'000'000;
    assert(scene.AdvanceAnimations(clock.now_ns));
    assert(Near(ProgressWidth(BuildAndCommit(scene), fill), 10));
    assert(!scene.HasActiveAnimations());
    assert(!scene.NextAnimationDeadlineNs(clock.now_ns));
    const auto finished_pixels = scene.PixelsRevision();
    clock.now_ns += 1'000'000'000;
    assert(!scene.AdvanceAnimations(clock.now_ns));
    assert(!scene.Build({1}));
    assert(scene.PixelsRevision() == finished_pixels);
    assert(scene.AnimationSample().time_ns == clock.now_ns - 1'000'000'000);
}

void CheckPremultipliedLinearColor()
{
    FakeClock clock;
    runtime::Scene scene(
        runtime::ParseBlueprint("Text(\"A\",foreground:$ink,font:20)"
                                ".transition(property:\"foreground\",durationMs:100,"
                                "easing:\"linear\")"),
        Shape);
    assert(scene.SetViewport({40, 30}));
    assert(scene.SetBinding("ink", contracts::Color{255, 0, 0, 0}));
    BuildAndCommit(scene);
    scene.EnableAnimations(&clock);

    const auto before = scene.PixelsRevision();
    assert(scene.SetBinding("ink", contracts::Color{0, 0, 255, 255}));
    assert(scene.PixelsRevision() == before);
    clock.now_ns += 50'000'000;
    assert(scene.AdvanceAnimations(clock.now_ns));
    assert(GlyphColor(BuildAndCommit(scene)) == (contracts::Color{0, 0, 255, 128}));

    clock.now_ns += 50'000'000;
    assert(scene.AdvanceAnimations(clock.now_ns));
    assert(GlyphColor(BuildAndCommit(scene)) == (contracts::Color{0, 0, 255, 255}));
    assert(!scene.HasActiveAnimations());

    // Opaque colors interpolate in linear light before sRGB encoding.
    assert(scene.SetBinding("ink", contracts::Color{0, 0, 0, 255}));
    clock.now_ns += 50'000'000;
    assert(scene.AdvanceAnimations(clock.now_ns));
    assert(GlyphColor(BuildAndCommit(scene)) == (contracts::Color{0, 0, 188, 255}));
    clock.now_ns += 50'000'000;
    assert(scene.AdvanceAnimations(clock.now_ns));
    assert(GlyphColor(BuildAndCommit(scene)) == (contracts::Color{0, 0, 0, 255}));
}

void CheckDetachedTransitionProvenance()
{
    FakeClock clock;
    auto blueprint =
        runtime::ParseBlueprint("VStack { Progress(value:$progress,width:100,height:10)"
                                ".transition(property:\"value\",durationMs:200,easing:\"linear\")"
                                " Card(width:100,height:20) }");
    blueprint.children[1].region = "slot";
    runtime::Scene scene(std::move(blueprint), Shape);
    assert(scene.SetViewport({100, 30}));
    assert(scene.SetBinding("progress", 0.2));
    BuildAndCommit(scene);
    scene.EnableAnimations(&clock);
    assert(scene.SetBinding("progress", 0.8));

    const runtime::RegionUpdate update{"slot",
                                       runtime::ParseBlueprint("Card(width:100,height:20)")};
    auto detached = scene.RegionBlueprint(std::span(&update, 1));
    detached.children[0].transitions.front().duration_ms = 300;
    runtime::Scene forged(std::move(detached), Shape);
    const auto transaction = scene.TransactionRevision();
    const auto pixels = scene.PixelsRevision();
    std::string diagnostic;
    assert(!scene.MountRegions(std::span(&update, 1), {}, forged, transaction, &diagnostic));
    assert(!diagnostic.empty());
    assert(scene.TransactionRevision() == transaction);
    assert(scene.PixelsRevision() == pixels);
    assert(!scene.RegionMounted("slot"));
    assert(scene.HasActiveAnimations());

    clock.now_ns += 50'000'000;
    assert(scene.AdvanceAnimations(clock.now_ns));
    BuildAndCommit(scene);
}

void CheckSceneRejectsMalformedDescriptors()
{
    auto blueprint =
        runtime::ParseBlueprint("Progress(value:0.2).transition(property:\"value\",durationMs:100,"
                                "easing:\"linear\")");
    blueprint.transitions.front().easing = static_cast<animation::Easing>(100);
    bool rejected = false;
    try {
        runtime::Scene scene(std::move(blueprint), Shape);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
}

void CheckThemeAndVisibilityBoundaries()
{
    constexpr contracts::Color fill{200, 117, 33, 255};
    constexpr auto source = "Progress(value:$progress,width:100,height:10,foreground:\"@accent\")"
                            ".transition(property:\"value\",durationMs:200,easing:\"linear\")";
    FakeClock clock;
    runtime::Scene themed(runtime::ParseBlueprint(source), Shape, {}, Theme(1));
    assert(themed.SetViewport({100, 10}));
    assert(themed.SetBinding("progress", 0.2));
    BuildAndCommit(themed);
    themed.EnableAnimations(&clock);
    assert(themed.SetBinding("progress", 0.8));
    clock.now_ns += 50'000'000;
    assert(themed.AdvanceAnimations(clock.now_ns));
    assert(Near(ProgressWidth(BuildAndCommit(themed), fill), 35));

    auto invalid = Theme(2);
    invalid.colors.clear();
    const auto before_pixels = themed.PixelsRevision();
    const auto before_transaction = themed.TransactionRevision();
    std::string diagnostic;
    assert(!themed.ApplyTheme(invalid, &diagnostic));
    assert(!diagnostic.empty() && themed.ThemeGeneration() == 1);
    assert(themed.HasActiveAnimations());
    assert(themed.PixelsRevision() == before_pixels);
    assert(themed.TransactionRevision() == before_transaction);
    assert(!themed.Build({1}));

    clock.now_ns += 50'000'000;
    assert(themed.AdvanceAnimations(clock.now_ns));
    assert(Near(ProgressWidth(BuildAndCommit(themed), fill), 50));
    assert(themed.ApplyTheme(Theme(2), &diagnostic) && diagnostic.empty());
    assert(themed.ThemeGeneration() == 2 && !themed.HasActiveAnimations());
    assert(Near(ProgressWidth(BuildAndCommit(themed), fill), 80));
    clock.now_ns += 200'000'000;
    assert(!themed.AdvanceAnimations(clock.now_ns));
    assert(!themed.Build({1}));

    runtime::Scene concealed(
        runtime::ParseBlueprint(
            "Card(width:100,height:20,visible:$show) {"
            " Progress(value:$progress,width:100,height:10,foreground:#C87521FF)"
            ".transition(property:\"value\",durationMs:200,easing:\"linear\")"
            " }"),
        Shape);
    assert(concealed.SetViewport({100, 20}));
    assert(concealed.SetBinding("progress", 0.2));
    BuildAndCommit(concealed);
    concealed.EnableAnimations(&clock);
    assert(concealed.SetBinding("progress", 0.8));
    clock.now_ns += 50'000'000;
    assert(concealed.AdvanceAnimations(clock.now_ns));
    assert(Near(ProgressWidth(BuildAndCommit(concealed), fill), 35));

    assert(concealed.SetBinding("show", false));
    assert(!concealed.HasActiveAnimations());
    BuildAndCommit(concealed);
    const auto hidden_pixels = concealed.PixelsRevision();
    assert(concealed.SetBinding("progress", 0.4));
    assert(concealed.PixelsRevision() == hidden_pixels);
    clock.now_ns += 1'000'000'000;
    assert(!concealed.AdvanceAnimations(clock.now_ns));
    assert(!concealed.NextAnimationDeadlineNs(clock.now_ns));
    assert(concealed.PixelsRevision() == hidden_pixels);

    assert(concealed.SetBinding("show", true));
    assert(Near(ProgressWidth(BuildAndCommit(concealed), fill), 40));
    assert(!concealed.HasActiveAnimations());
    clock.now_ns += 1'000'000'000;
    assert(!concealed.AdvanceAnimations(clock.now_ns));
    assert(!concealed.Build({1}));
}

void CheckNamedMotion()
{
    constexpr contracts::Color fill{200, 117, 33, 255};
    constexpr auto source = "Progress(value:$progress,width:100,height:10,foreground:#C87521FF)"
                            ".transition(property:\"value\",motion:\"control.feedback\")";
    auto theme = Theme(1);
    theme.schema_version = 3;
    theme.motion = {"fixture", {{"control.feedback", 200, contracts::MotionEasing::Linear}}};
    FakeClock clock;
    runtime::Scene scene(runtime::ParseBlueprint(source), Shape, {}, theme);
    assert(scene.SetViewport({100, 10}));
    assert(scene.SetBinding("progress", 0.2));
    BuildAndCommit(scene);
    scene.EnableAnimations(&clock);
    assert(scene.SetBinding("progress", 0.8));
    clock.now_ns += 50'000'000;
    assert(scene.AdvanceAnimations(clock.now_ns));
    assert(Near(ProgressWidth(BuildAndCommit(scene), fill), 35));

    auto invalid = theme;
    invalid.generation = 2;
    invalid.motion.transitions.front().name = "missing";
    std::string diagnostic;
    const auto revision = scene.TransactionRevision();
    assert(!scene.ApplyTheme(invalid, &diagnostic));
    assert(!diagnostic.empty() && scene.TransactionRevision() == revision);
    assert(scene.HasActiveAnimations());

    theme.generation = 2;
    theme.motion.id = "instant";
    theme.motion.transitions.front().duration_ms = 0;
    assert(scene.ApplyTheme(theme));
    assert(!scene.HasActiveAnimations());
    assert(Near(ProgressWidth(BuildAndCommit(scene), fill), 80));
    assert(scene.SetBinding("progress", 0.3));
    assert(Near(ProgressWidth(BuildAndCommit(scene), fill), 30));
    assert(!scene.HasActiveAnimations());

    bool rejected{};
    try {
        runtime::Scene absent(runtime::ParseBlueprint(source), Shape);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
    rejected = false;
    try {
        runtime::ParseBlueprint("Progress(value:0).transition(property:\"value\","
                                "motion:\"control.feedback\",durationMs:100,easing:\"linear\")");
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);
}
} // namespace

int main()
{
    CheckNamedMotion();
    CheckProgressAndRetarget();
    CheckPremultipliedLinearColor();
    CheckDetachedTransitionProvenance();
    CheckSceneRejectsMalformedDescriptors();
    CheckThemeAndVisibilityBoundaries();
}
