#include "prism/animation/timeline.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace prism;
using namespace prism::runtime;

namespace {
constexpr contracts::WindowId window{1};
constexpr contracts::InputSource pointer{1, 1, 1};
constexpr contracts::InputSource keyboard{1, 2, 1};
constexpr contracts::Color baseColor{40, 80, 120, 255};
constexpr contracts::Color hoverColor{90, 130, 170, 255};
constexpr contracts::Color nextColor{20, 160, 80, 255};

class FakeClock final : public animation::AnimationClock {
public:
    std::uint64_t now_ns{1'000'000'000};

    std::uint64_t NowNs() const noexcept override
    {
        return now_ns;
    }
};

ShapedText Shape(std::string_view text, double size)
{
    ShapedText shaped;
    shaped.width = text.size() * size * 0.5;
    shaped.height = size;
    for (std::size_t index = 0; index < text.size(); ++index) {
        shaped.glyphs.push_back(
            {static_cast<std::uint32_t>(text[index]), {index * size * 0.5, size}});
    }
    return shaped;
}

contracts::DisplayList Build(Scene &scene)
{
    auto list = scene.Build(window);
    assert(list);
    scene.AcknowledgeComposite();
    return std::move(*list);
}

void Layout(Scene &scene, contracts::LogicalSize size = {100, 40})
{
    assert(scene.SetViewport(size));
    Build(scene);
}

InteractionResult Move(Scene &scene, contracts::LogicalPoint position)
{
    return scene.HandleInput(contracts::PointerMotionEvent{window, position, 1, pointer});
}

InteractionResult Button(Scene &scene, contracts::LogicalPoint position,
                         contracts::ButtonState state)
{
    return scene.HandleInput(contracts::PointerButtonEvent{
        window, position, contracts::PointerButton::Primary, state, 0, 1, pointer});
}

InteractionResult Down(Scene &scene, contracts::LogicalPoint position = {20, 10})
{
    return Button(scene, position, contracts::ButtonState::Pressed);
}

InteractionResult Up(Scene &scene, contracts::LogicalPoint position = {20, 10})
{
    return Button(scene, position, contracts::ButtonState::Released);
}

bool Near(double actual, double expected)
{
    return std::abs(actual - expected) < 0.0001;
}

struct Paint {
    std::array<double, 6> transform{1, 0, 0, 0, 1, 0};
    double opacity{1};
};

Paint PaintFor(const contracts::DisplayList &list, contracts::Color color)
{
    std::vector<std::array<double, 6>> transforms{{1, 0, 0, 0, 1, 0}};
    std::vector<double> opacities{1};
    for (const auto &command : list.commands) {
        if (const auto *transform = std::get_if<contracts::PushTransform>(&command)) {
            transforms.push_back(transform->values);
        } else if (std::holds_alternative<contracts::PopTransform>(command)) {
            assert(transforms.size() > 1);
            transforms.pop_back();
        } else if (const auto *opacity = std::get_if<contracts::PushOpacity>(&command)) {
            opacities.push_back(opacity->opacity);
        } else if (std::holds_alternative<contracts::PopOpacity>(command)) {
            assert(opacities.size() > 1);
            opacities.pop_back();
        } else if (const auto *fill = std::get_if<contracts::FillRect>(&command)) {
            if (fill->color == color) {
                return {transforms.back(), opacities.back()};
            }
        } else if (const auto *fill = std::get_if<contracts::FillRoundedRect>(&command)) {
            if (fill->color == color) {
                return {transforms.back(), opacities.back()};
            }
        }
    }
    assert(false);
    return {};
}

contracts::ThemeSnapshot Theme(std::uint64_t generation = 1)
{
    contracts::ThemeSnapshot theme;
    theme.id = "state-fixture";
    theme.name = "State fixture";
    theme.generation = generation;
    theme.colors = {{"base", baseColor}, {"hover", hoverColor}};
    theme.numbers = {{"hoverScale", 1.2}};
    return theme;
}

void CheckBindingsAndPriority()
{
    Scene scene(ParseBlueprint(R"(
        Card {
            InteractionTarget(width:80,height:32,action:"go") {
                Visual(width:40,height:4,background:$ink,scaleX:$baseScale)
                    .state(when:"hovered",scope:"target",scaleX:1.2,background:#5A82AAFF)
                    .state(when:"captured",scope:"target",scaleX:1.05)
                    .state(when:"pressed",scope:"target",scaleX:0.8)
                    .state(when:"disabled",scope:"target",opacity:0.4)
            }
        }
    )"),
                Shape);
    assert(scene.SetBinding("ink", baseColor));
    assert(scene.SetBinding("baseScale", 0.95));
    Layout(scene);
    const auto target = scene.HitTest({70, 25})->node;
    const auto regions = scene.InputRegions();
    const auto layouts = scene.GetRenderStats().layouts;
    assert(scene.ActionAt({70, 25}) == "go"); // visual is much smaller than its target

    assert(Move(scene, {70, 25}).changed);
    auto hovered = Build(scene);
    assert(Near(PaintFor(hovered, hoverColor).transform[0], 1.2));
    assert(scene.SetBinding("ink", nextColor));
    assert(scene.SetBinding("baseScale", 1.1));
    if (auto changed = scene.Build(window)) {
        hovered = std::move(*changed);
        scene.AcknowledgeComposite();
    }
    assert(Near(PaintFor(hovered, hoverColor).transform[0], 1.2));

    Down(scene, {70, 25});
    assert(Near(PaintFor(Build(scene), hoverColor).transform[0], 0.8));
    Move(scene, {95, 35});
    assert(scene.State(target).captured && !scene.State(target).pressed);
    assert(Near(PaintFor(Build(scene), nextColor).transform[0], 1.05));
    assert(!Up(scene, {95, 35}).activation);
    assert(Near(PaintFor(Build(scene), nextColor).transform[0], 1.1));

    assert(scene.SetEnabled(target, false));
    assert(Near(PaintFor(Build(scene), nextColor).opacity, 0.4));
    assert(!scene.HitTest({70, 25}));
    assert(scene.SetEnabled(target, true));
    assert(Near(PaintFor(Build(scene), nextColor).opacity, 1));
    assert(scene.GetRenderStats().layouts == layouts);
    assert(scene.InputRegions() == regions);
    assert(scene.ActionAt({70, 25}) == "go");
    assert(!scene.ActionAt({80, 25}));
}

void CheckTimelineAndStableInput()
{
    FakeClock clock;
    Scene scene(ParseBlueprint(R"(
        Card {
            InteractionTarget(width:80,height:32,action:"go") {
                Visual(width:40,height:4,background:#285078FF)
                    .state(when:"hovered",scope:"target",scaleX:1.2)
                    .state(when:"pressed",scope:"target",scaleX:0.8)
                    .transition(property:"scaleX",durationMs:200,easing:"linear")
            }
        }
    )"),
                Shape);
    Layout(scene);
    scene.EnableAnimations(&clock);
    const auto regions = scene.InputRegions();
    const auto target = scene.HitTest({70, 25})->node;
    const auto bounds = scene.Bounds(target);
    const auto layouts = scene.GetRenderStats().layouts;

    Move(scene, {70, 25});
    assert(scene.HasActiveAnimations());
    clock.now_ns += 50'000'000;
    assert(scene.AdvanceAnimations(clock.now_ns));
    assert(Near(PaintFor(Build(scene), baseColor).transform[0], 1.05));
    assert(!Move(scene, {71, 25}).changed); // continuous movement must not restart the timeline

    clock.now_ns += 50'000'000;
    Down(scene, {71, 25});
    assert(Near(PaintFor(Build(scene), baseColor).transform[0], 1.1));
    clock.now_ns += 50'000'000;
    assert(scene.AdvanceAnimations(clock.now_ns));
    assert(Near(PaintFor(Build(scene), baseColor).transform[0], 1.025));
    clock.now_ns += 150'000'000;
    assert(scene.AdvanceAnimations(clock.now_ns));
    assert(Near(PaintFor(Build(scene), baseColor).transform[0], 0.8));
    assert(!scene.HasActiveAnimations());
    assert(scene.Bounds(target) == bounds && scene.InputRegions() == regions);
    assert(scene.ActionAt({71, 25}) == "go");
    assert(scene.GetRenderStats().layouts == layouts);

    const auto released = Up(scene, {71, 25});
    assert(released.activation && released.activation->node == target);
    clock.now_ns += 200'000'000;
    assert(scene.AdvanceAnimations(clock.now_ns));
    assert(Near(PaintFor(Build(scene), baseColor).transform[0], 1.2));
    Move(scene, {95, 35});
    clock.now_ns += 200'000'000;
    assert(scene.AdvanceAnimations(clock.now_ns));
    assert(Near(PaintFor(Build(scene), baseColor).transform[0], 1));
    assert(!scene.HasActiveAnimations());
    assert(!scene.NextAnimationDeadlineNs(clock.now_ns));
    assert(!scene.Build(window));
}

void CheckIndependentTargetsAndFocus()
{
    Scene scene(ParseBlueprint(R"(
        HStack(spacing:20) {
            InteractionTarget(width:80,height:32,action:"shared") {
                Visual(width:40,height:4,background:#285078FF)
                    .state(when:"hovered",scope:"target",scaleX:1.2)
                    .state(when:"focusVisible",scope:"target",opacity:0.5)
            }
            InteractionTarget(width:80,height:32,action:"shared") {
                Visual(width:40,height:4,background:#14A050FF)
                    .state(when:"hovered",scope:"target",scaleX:1.4)
                    .state(when:"focusVisible",scope:"target",opacity:0.6)
            }
        }
    )"),
                Shape);
    Layout(scene, {180, 40});
    const auto first = scene.HitTest({20, 10})->node;
    const auto second = scene.HitTest({120, 10})->node;
    assert(first != second);
    Move(scene, {120, 10});
    const auto hovered = Build(scene);
    assert(Near(PaintFor(hovered, baseColor).transform[0], 1));
    assert(Near(PaintFor(hovered, nextColor).transform[0], 1.4));

    scene.HandleInput(contracts::FocusEvent{window, true, keyboard});
    scene.HandleInput(
        contracts::KeyEvent{window, 0x2b, contracts::ButtonState::Pressed, false, 1, keyboard, {}});
    const auto focused = Build(scene);
    assert(scene.State(first).focusVisible && scene.State(second).hovered);
    assert(Near(PaintFor(focused, baseColor).opacity, 0.5));
    assert(Near(PaintFor(focused, nextColor).opacity, 1));
    assert(Near(PaintFor(focused, nextColor).transform[0], 1.4));
}

void CheckActionlessTargetAndNearestScope()
{
    Scene scene(ParseBlueprint(R"(
        InteractionTarget(width:100,height:80) {
            VStack(spacing:20) {
                Visual(width:40,height:4,background:#285078FF)
                    .state(when:"hovered",scope:"target",scaleX:1.2)
                InteractionTarget(width:80,height:32,action:"inner") {
                    Visual(width:40,height:4,background:#14A050FF)
                        .state(when:"hovered",scope:"target",scaleX:1.4)
                }
            }
        }
    )"),
                Shape);
    Layout(scene, {100, 80});
    const auto outer = scene.HitTest({90, 70})->node;
    const auto inner = scene.HitTest({20, 30})->node;
    assert(outer == scene.RootId() && inner != outer);
    assert(!scene.ActionAt({90, 70}));
    Move(scene, {90, 70});
    const auto outerHover = Build(scene);
    assert(Near(PaintFor(outerHover, baseColor).transform[0], 1.2));
    assert(Near(PaintFor(outerHover, nextColor).transform[0], 1));
    Down(scene, {90, 70});
    assert(scene.State(outer).pressed && scene.State(outer).captured);
    assert(!Up(scene, {90, 70}).activation);

    Move(scene, {20, 30});
    const auto innerHover = Build(scene);
    assert(Near(PaintFor(innerHover, baseColor).transform[0], 1));
    assert(Near(PaintFor(innerHover, nextColor).transform[0], 1.4));
    Down(scene, {20, 30});
    const auto activation = Up(scene, {20, 30});
    assert(activation.activation && activation.activation->action == "inner");
}

contracts::Color IconColor(const contracts::DisplayList &list)
{
    for (const auto &command : list.commands) {
        if (const auto *icon = std::get_if<contracts::DrawIcon>(&command)) {
            return icon->color;
        }
    }
    assert(false);
    return {};
}

void CheckForegroundAndInvisiblePresentation()
{
    Scene scene(ParseBlueprint(R"(
        Card {
            InteractionTarget(width:80,height:32,action:"go") {
                Visual(width:40,height:20)
                {
                    Icon("play",foreground:$ink)
                        .state(when:"hovered",scope:"target",foreground:#5A82AAFF)
                }
                .state(when:"pressed",scope:"target",opacity:0)
            }
        }
    )"),
                Shape);
    assert(scene.SetBinding("ink", baseColor));
    Layout(scene);
    const auto regions = scene.InputRegions();
    Move(scene, {20, 10});
    assert(IconColor(Build(scene)) == hoverColor);
    assert(scene.SetBinding("ink", nextColor));
    if (auto changed = scene.Build(window)) {
        assert(IconColor(*changed) == hoverColor);
        scene.AcknowledgeComposite();
    }

    Down(scene);
    Build(scene);
    assert(scene.ActionAt({20, 10}) == "go");
    assert(scene.InputRegions() == regions); // zero visual opacity does not release capture
    assert(Up(scene).activation);
    Move(scene, {95, 35});
    assert(IconColor(Build(scene)) == nextColor);
}

void CheckThemeAtomicity()
{
    FakeClock clock;
    Scene scene(ParseBlueprint(R"(
        Card {
            InteractionTarget(width:80,height:32,action:"go") {
                Visual(width:40,height:4,background:"@base")
                    .state(when:"hovered",scope:"target",background:"@hover",scaleX:"@hoverScale")
                    .transition(property:"scaleX",durationMs:200,easing:"linear")
            }
        }
    )"),
                Shape, {}, Theme());
    Layout(scene);
    scene.EnableAnimations(&clock);
    Move(scene, {20, 10});
    clock.now_ns += 50'000'000;
    assert(scene.AdvanceAnimations(clock.now_ns));
    assert(Near(PaintFor(Build(scene), hoverColor).transform[0], 1.05));

    auto invalid = Theme(2);
    invalid.colors.pop_back(); // a state-only reference is part of atomic theme validation
    const auto pixels = scene.PixelsRevision();
    const auto transaction = scene.TransactionRevision();
    std::string diagnostic;
    assert(!scene.ApplyTheme(invalid, &diagnostic));
    assert(!diagnostic.empty() && scene.ThemeGeneration() == 1);
    assert(scene.PixelsRevision() == pixels && scene.TransactionRevision() == transaction);
    assert(scene.HasActiveAnimations() && !scene.Build(window));

    auto changed = Theme(2);
    changed.colors[0].value = nextColor;
    changed.colors[1].value = {160, 100, 200, 255};
    changed.numbers[0].value = 1.4;
    assert(scene.ApplyTheme(changed, &diagnostic));
    assert(diagnostic.empty() && !scene.HasActiveAnimations());
    assert(Near(PaintFor(Build(scene), changed.colors[1].value).transform[0], 1.4));
    Move(scene, {95, 35});
    clock.now_ns += 200'000'000;
    assert(scene.AdvanceAnimations(clock.now_ns));
    assert(Near(PaintFor(Build(scene), nextColor).transform[0], 1));
}

void CheckVisibilityCancellation()
{
    FakeClock clock;
    Scene scene(ParseBlueprint(R"(
        Card(visible:$show) {
            InteractionTarget(width:80,height:32,action:"go") {
                Visual(width:40,height:4,background:#285078FF)
                    .state(when:"hovered",scope:"target",scaleX:1.2)
                    .transition(property:"scaleX",durationMs:200,easing:"linear")
            }
        }
    )"),
                Shape);
    Layout(scene);
    scene.EnableAnimations(&clock);
    Move(scene, {20, 10});
    clock.now_ns += 50'000'000;
    assert(scene.AdvanceAnimations(clock.now_ns));
    Build(scene);
    assert(scene.HasActiveAnimations());
    assert(scene.SetBinding("show", false));
    assert(!scene.HasActiveAnimations());
    Build(scene);
    clock.now_ns += 1'000'000'000;
    assert(!scene.AdvanceAnimations(clock.now_ns));
    assert(!scene.NextAnimationDeadlineNs(clock.now_ns));
    assert(!scene.Build(window));
    scene.CancelInput();
    assert(scene.SetBinding("show", true));
    assert(Near(PaintFor(Build(scene), baseColor).transform[0], 1));
}

Blueprint RegionLayout()
{
    auto layout = ParseBlueprint(R"(
        HStack(spacing:20) {
            InteractionTarget(width:80,height:32,action:"retained") {
                Visual(width:40,height:4,background:#285078FF)
                    .state(when:"hovered",scope:"target",scaleX:1.2)
                    .transition(property:"scaleX",durationMs:200,easing:"linear")
            }
            Card(width:80,height:32) {
                InteractionTarget(width:80,height:32,action:"old") {
                    Visual(width:40,height:4,background:#14A050FF)
                        .state(when:"hovered",scope:"target",scaleX:1.4)
                        .transition(property:"scaleX",durationMs:200,easing:"linear")
                }
            }
        }
    )");
    layout.children[1].region = "body";
    return layout;
}

void CheckRegionStateLifetimes()
{
    FakeClock clock;
    Scene scene(RegionLayout(), Shape);
    Layout(scene, {180, 40});
    scene.EnableAnimations(&clock);
    const auto retained = scene.HitTest({20, 10})->node;
    Move(scene, {20, 10});
    clock.now_ns += 50'000'000;
    assert(scene.AdvanceAnimations(clock.now_ns));
    Build(scene);
    const RegionUpdate update{"body", ParseBlueprint(R"(
        InteractionTarget(width:80,height:32,action:"new") {
            Visual(width:40,height:4,background:#14A050FF)
                .state(when:"hovered",scope:"target",scaleX:1.6)
        }
    )")};

    auto detached = scene.RegionBlueprint(std::span(&update, 1));
    detached.children[0].children[0].state_rules[0].properties[0].value = 1.8;
    Scene forged(std::move(detached), Shape);
    const auto transaction = scene.TransactionRevision();
    const auto pixels = scene.PixelsRevision();
    std::string diagnostic;
    assert(!scene.MountRegions(std::span(&update, 1), {}, forged, transaction, &diagnostic));
    assert(!diagnostic.empty() && !scene.RegionMounted("body"));
    assert(scene.TransactionRevision() == transaction && scene.PixelsRevision() == pixels);

    assert(scene.MountRegions(std::span(&update, 1), {}));
    assert(scene.State(retained).hovered && scene.HasActiveAnimations());
    assert(Near(PaintFor(Build(scene), baseColor).transform[0], 1.05));
    clock.now_ns += 150'000'000;
    assert(scene.AdvanceAnimations(clock.now_ns));
    assert(Near(PaintFor(Build(scene), baseColor).transform[0], 1.2));

    Scene replaced(RegionLayout(), Shape);
    Layout(replaced, {180, 40});
    replaced.EnableAnimations(&clock);
    const auto old = replaced.HitTest({120, 10})->node;
    Down(replaced, {120, 10});
    assert(replaced.HasActiveAnimations() && replaced.State(old).captured);
    assert(replaced.MountRegions(std::span(&update, 1), {}));
    Build(replaced);
    assert(!replaced.State(old).enabled && !replaced.State(old).captured);
    assert(!replaced.HasActiveAnimations());
    assert(!Up(replaced, {120, 10}).activation);
    assert(replaced.HitTest({120, 10})->node != old);
}

void CheckMalformedBlueprint()
{
    auto source = ParseBlueprint(R"(
        InteractionTarget { Visual.state(when:"hovered",scope:"target",scaleX:1.2) }
    )");
    source.children[0].state_rules[0].condition = static_cast<StateCondition>(999);
    bool rejected = false;
    try {
        Scene scene(std::move(source), Shape);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
}
} // namespace

int main()
{
    CheckBindingsAndPriority();
    CheckTimelineAndStableInput();
    CheckIndependentTargetsAndFocus();
    CheckActionlessTargetAndNearestScope();
    CheckForegroundAndInvisiblePresentation();
    CheckThemeAtomicity();
    CheckVisibilityCancellation();
    CheckRegionStateLifetimes();
    CheckMalformedBlueprint();
}
