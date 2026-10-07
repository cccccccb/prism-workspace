#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/sdk/module_session.hpp"
#include <cassert>
#include <functional>

using namespace prism;
using namespace prism::runtime;

namespace {
constexpr auto source = R"(
VStack(spacing: 0) {
    RadioGroup(action: "scheme", selectedKey: $scheme, spacing: 0, enabled: $available) {
        Radio("light", height: 44) { Visual(background: #111111FF).state(when: "selected", scope: "target", background: #777777FF) }
        Radio("system", height: 44, enabled: false) { Visual }
        Radio("dark", height: 44, visible: $showDark) { Visual }
    }
    SegmentGroup(action: "mode", selectedKey: $mode, height: 44) {
        Segment("compact", flex: 1) { Visual }
        Segment("comfortable", flex: 1) { Visual }
    }
    Button("After", action: "after", height: 44)
}
)";

ShapedText Shape(std::string_view text, double size)
{
    return {{}, text.size() * 7.0, size};
}

contracts::NodeId Option(Scene &scene, std::string_view action, std::size_t index)
{
    for (const auto &node : scene.InputGeometry()->nodes) {
        if (node.action == action && index-- == 0) {
            return node.id;
        }
    }
    assert(false);
    return {};
}

InteractionResult Key(Scene &scene, std::uint32_t key, bool down = true, bool shift = false,
                      bool repeat = false)
{
    contracts::KeyEvent event{
        {1},    key, down ? contracts::ButtonState::Pressed : contracts::ButtonState::Released,
        repeat, 1,   {1, 2, 1},
        {}};
    event.modifiers.shift = shift;
    return scene.HandleInput(event, scene.InputGeometry());
}

InteractionResult Button(Scene &scene, contracts::NodeId id, bool down)
{
    const auto bounds = scene.Bounds(id);
    return scene.HandleInput(
        contracts::PointerButtonEvent{{1},
                                      {bounds.x + bounds.width / 2, bounds.y + bounds.height / 2},
                                      contracts::PointerButton::Primary,
                                      down ? contracts::ButtonState::Pressed
                                           : contracts::ButtonState::Released,
                                      0,
                                      1,
                                      {1, 1, 1}},
        scene.InputGeometry());
}

void Prepare(Scene &scene)
{
    scene.SetBinding("available", true);
    scene.SetBinding("showDark", true);
    scene.SetBinding("scheme", std::string("light"));
    scene.SetBinding("mode", std::string("compact"));
    scene.SetViewport({320, 264});
    assert(scene.Build({1}));
}

void KeyboardAndBusiness(const char *module_path)
{
    Scene scene(ParseBlueprint(source), Shape);
    Prepare(scene);
    const auto light = Option(scene, "scheme", 0), dark = Option(scene, "scheme", 2);
    const auto compact = Option(scene, "mode", 0), comfortable = Option(scene, "mode", 1);
    sdk::ModuleSession module(module_path, "choice.fixture", 1,
                              std::bind_front(&Scene::SetBinding, &scene));
    assert(module.Start());

    assert(scene.State(light).selected && !scene.State(dark).selected);
    Key(scene, 0x2b);
    assert(scene.State(light).focusVisible);
    auto result = Key(scene, 0x4f); // Right skips disabled System.
    assert(result.control_edit && !result.activation);
    assert(result.control_edit->node == dark);
    assert(std::get<std::string>(result.control_edit->event.before) == "light");
    assert(std::get<std::string>(result.control_edit->event.value) == "dark");
    assert(scene.State(dark).focused && scene.State(light).selected); // No optimistic selection.
    assert(scene.IsCurrentControlEdit(*result.control_edit));
    module.ControlValue(*result.control_edit);
    assert(!scene.IsCurrentControlEdit(*result.control_edit));
    assert(scene.State(dark).selected && !scene.State(light).selected);
    assert(scene.Build({1}));
    assert(!Key(scene, 0x4d).control_edit);                    // End already selected.
    assert(!Key(scene, 0x4f, true, false, true).control_edit); // Ignore repeats.

    Key(scene, 0x2b);
    assert(scene.State(compact).focused); // Whole Radio group is one Tab stop.
    result = Key(scene, 0x4d);
    assert(result.control_edit && result.control_edit->node == comfortable);
    module.ControlValue(*result.control_edit);
    Key(scene, 0x2b);
    assert(scene.State(Option(scene, "after", 0)).focused);
    Key(scene, 0x2b, true, true);
    assert(scene.State(comfortable).focused);
    Key(scene, 0x2b, true, true);
    assert(scene.State(dark).focused);

    result = Key(scene, 0x4a);
    assert(result.control_edit && result.control_edit->node == light);
    module.ControlValue(*result.control_edit);
    result = Key(scene, 0x50); // Left wraps to last available option.
    assert(result.control_edit && result.control_edit->node == dark);
    // Rejected proposal keeps selection, but Tab still exits the group from focused Dark.
    Key(scene, 0x2b);
    assert(scene.State(comfortable).focused);
}

void PointerAndCancellation()
{
    Scene scene(ParseBlueprint(source), Shape);
    Prepare(scene);
    const auto light = Option(scene, "scheme", 0), dark = Option(scene, "scheme", 2);
    Button(scene, light, true);
    assert(!Button(scene, light, false).control_edit);
    Button(scene, dark, true);
    auto result = Button(scene, dark, false);
    assert(result.control_edit && result.control_edit->action == "scheme" && !result.activation);
    assert(!Button(scene, dark, false).control_edit);

    Button(scene, dark, true);
    scene.SetBinding("scheme", std::string("dark"));
    scene.SetBinding("scheme", std::string("light"));
    assert(!Button(scene, dark, false).control_edit); // Group revision prevents ABA release.
    Button(scene, dark, true);
    assert(scene.Preflight({{"scheme", std::string("dark")}}));
    assert(!Button(scene, dark, false).control_edit);
    scene.SetBinding("scheme", std::string("light"));
    Button(scene, dark, true);
    Key(scene, 0x29);
    assert(!Button(scene, dark, false).control_edit);
    Button(scene, dark, true);
    scene.SetBinding("available", false);
    assert(!Button(scene, dark, false).control_edit);
    assert(!scene.IsCurrentControlEdit(*result.control_edit));
    scene.SetBinding("available", true);
    Button(scene, dark, true);
    scene.SetBinding("showDark", false);
    assert(!Button(scene, dark, false).control_edit);
    assert(scene.Build({1}));
    Key(scene, 0x2b);
    assert(scene.State(light).focused);
    assert(!Key(scene, 0x4f).control_edit); // The sole available choice stays selected.

    assert(!scene.SetBinding("scheme", std::string("unknown")));
    assert(!scene.AcceptsBinding("scheme", true));
    assert(!scene.SetProperty(light, DslProperty::OptionKey, std::string("new")));
    assert(scene.SetBinding("scheme", std::string{}));
    assert(!scene.State(light).selected);
    Button(scene, light, true);
    assert(!Button(scene, light, false).control_edit); // Cleared group awaits authoritative value.
    scene.SetBinding("scheme", std::string("system")); // Disabled value may remain authoritative.
    assert(scene.Build({1}));
    Key(scene, 0x2b, true, true);
    // Verify a fresh focus traversal chooses an enabled option, never disabled System.
    scene.CancelInput();
    Key(scene, 0x2b);
    assert(scene.State(light).focused);
}

void Reject(std::string_view source)
{
    bool rejected = false;
    try {
        Scene scene(ParseBlueprint(source), Shape);
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);
}

void Validation()
{
    Reject("Radio(\"light\")");
    Reject("RadioGroup {}");
    Reject("RadioGroup { Radio(\"a\") Radio(\"a\") }");
    Reject("RadioGroup { Segment(\"a\") }");
    Reject("RadioGroup { Radio(key: $identity) }");
    Reject("RadioGroup { Radio(\"@identity\") }");
    Reject("RadioGroup { Radio(\"\") }");
    Reject("RadioGroup(selectedKey: \"missing\") { Radio(\"light\") }");
    Reject("RadioGroup { Radio(\"light\", action: \"bad\") }");
    Reject("RadioGroup { Radio(\"light\") }.transition(property: \"selectedKey\", durationMs: 20)");
    Reject("Card { Visual { RadioGroup { Radio(\"a\") } } }");

    Scene shared(ParseBlueprint(R"(VStack {
        Text($shared)
        RadioGroup(action: "pick", selectedKey: $shared) { Radio("a") Radio("b") }
        SegmentGroup(action: "pick", selectedKey: $shared) { Segment("a") Segment("c") }
    })"),
                 Shape);
    assert(shared.SetBinding("shared", std::string("a")));
    assert(!shared.SetBinding("shared", std::string("b"))); // Reject entire projection, not half.
    assert(!shared.SetBinding("shared", std::string("a")));
    assert(!shared.Preflight({{"shared", std::string("b")}}));
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2);
    Validation();
    KeyboardAndBusiness(argv[1]);
    PointerAndCancellation();
}
