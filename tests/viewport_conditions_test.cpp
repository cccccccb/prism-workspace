#include "prism/animation/timeline.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"
#include <cassert>
#include <stdexcept>

using namespace prism;

namespace {
class Clock final : public animation::AnimationClock {
public:
    std::uint64_t NowNs() const noexcept override
    {
        return 1000000000;
    }
};

runtime::ShapedText Shape(std::string_view text, double size)
{
    return {{}, text.size() * 7.0, size};
}
} // namespace

int main()
{
    runtime::Scene scene(runtime::ParseBlueprint(R"(
        VStack(spacing: 0) {
            InteractionTarget(action: "wide", height: 32, minViewportWidth: 360,
                              minViewportHeight: 200, visible: $enabled) {}
            InteractionTarget(action: "narrow", height: 32, maxViewportWidth: 360) {}
            InteractionTarget(action: "always", height: 32) {}
        }
    )"),
                         Shape);
    scene.SetBinding("enabled", true);
    scene.SetViewport({360, 200});
    assert(scene.Build({1}));
    assert(scene.ActionAt({16, 16}) == "wide");
    assert(scene.ActionAt({16, 48}) == "always");
    assert(scene.FocusNext() && scene.FocusedAction() == "wide");
    contracts::PointerButtonEvent press;
    press.position = {16, 16};
    press.state = contracts::ButtonState::Pressed;
    scene.HandleInput(press);
    auto before = scene.InputGeometry();

    scene.SetViewport({359, 200});
    assert(scene.Build({1}));
    assert(scene.ActionAt({16, 16}) == "narrow");
    assert(scene.ActionAt({16, 48}) == "always");
    assert(scene.FocusedAction() != "wide");
    press.state = contracts::ButtonState::Released;
    assert(!scene.HandleInput(press).activation);
    assert(before->nodes[1].visible); // Previously submitted snapshot stays immutable.
    assert(!scene.InputGeometry()->nodes[1].visible);
    assert(scene.InputGeometry()->version > before->version);

    scene.SetViewport({360, 199});
    assert(scene.Build({1}));
    assert(scene.ActionAt({16, 16}) == "always");
    scene.SetViewport({360, 200});
    scene.SetBinding("enabled", false);
    assert(scene.Build({1}));
    assert(scene.ActionAt({16, 16}) == "always");
    scene.SetBinding("enabled", true);
    assert(scene.Build({1}));
    assert(scene.ActionAt({16, 16}) == "wide");
    assert(!scene.Build({1}));

    assert(scene.SetProperty({1, 1}, runtime::DslProperty::MinViewportWidth, 400.0));
    assert(scene.Build({1}));
    assert(scene.ActionAt({16, 16}) == "always");
    assert(!scene.SetProperty({1, 1}, runtime::DslProperty::MinViewportWidth, -1.0));
    bool rejected = false;
    try {
        runtime::ParseBlueprint("Card(minViewportWidth: -1) {}");
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);

    // A resized-away branch must not keep requesting animation samples.
    Clock clock;
    runtime::Scene animated(runtime::ParseBlueprint(R"(
        Card {
            Progress(value: $value, minViewportWidth: 360)
                .transition(property: "value", durationMs: 200, easing: "linear")
        }
    )"),
                            Shape);
    animated.SetViewport({400, 200});
    animated.SetBinding("value", 0.2);
    assert(animated.Build({1}));
    animated.EnableAnimations(&clock);
    animated.SetBinding("value", 0.8);
    assert(animated.HasActiveAnimations());
    animated.SetViewport({350, 200});
    assert(!animated.HasActiveAnimations());
    assert(animated.Build({1}));
    assert(!animated.Build({1}));
    animated.SetViewport({400, 200});
    assert(animated.Build({1}));
    assert(!animated.HasActiveAnimations());
    animated.SetBinding("value", 0.1);
    assert(animated.HasActiveAnimations());
    assert(animated.SetProperty({1, 1}, runtime::DslProperty::MinViewportWidth, 500.0));
    assert(!animated.HasActiveAnimations());
}
