#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/sdk/module_session.hpp"
#include <cassert>
#include <functional>
#include <iostream>

using namespace prism;
using namespace prism::runtime;

namespace {
constexpr auto source = R"(
VStack(spacing: 0, enabled: $available) {
    Checkbox(action: "notify", checked: $notifications, height: 44, enabled: $canEdit) {
        Visual(width: 24, height: 24, background: #223344FF) {
            Icon("check", visible: $notifications, foreground: #FFFFFFFF)
        }.state(when: "pressed", scope: "target", background: #445566FF)
    }
    Toggle(action: "legacy", checked: false, height: 44)
}
)";

ShapedText Shape(std::string_view text, double size)
{
    return {{}, text.size() * 7.0, size};
}

InteractionResult Button(Scene &scene, bool down, contracts::LogicalPoint point = {12, 12})
{
    return scene.HandleInput(contracts::PointerButtonEvent{{1},
                                                           point,
                                                           contracts::PointerButton::Primary,
                                                           down ? contracts::ButtonState::Pressed
                                                                : contracts::ButtonState::Released,
                                                           0,
                                                           1,
                                                           {1, 1, 1}},
                             scene.InputGeometry());
}

InteractionResult Key(Scene &scene, std::uint32_t key, bool down, bool repeat = false)
{
    return scene.HandleInput(contracts::KeyEvent{{1},
                                                 key,
                                                 down ? contracts::ButtonState::Pressed
                                                      : contracts::ButtonState::Released,
                                                 repeat,
                                                 1,
                                                 {1, 2, 1},
                                                 {}},
                             scene.InputGeometry());
}

void Prepare(Scene &scene)
{
    scene.SetBinding("available", true);
    scene.SetBinding("notifications", false);
    scene.SetBinding("canEdit", true);
    scene.SetViewport({240, 120});
    assert(scene.Build({1}));
}

void PointerAndBusiness(const char *module_path)
{
    Scene scene(ParseBlueprint(source), Shape);
    Prepare(scene);
    const auto target = scene.HitTest({12, 12})->node;
    const auto snapshot = scene.InputGeometry();
    assert(!Button(scene, true).control_edit);
    const auto result = Button(scene, false);
    assert(result.control_edit && !result.activation);
    assert(result.control_edit->node == target);
    assert(scene.IsCurrentControlEdit(*result.control_edit));
    assert(result.control_edit->event.phase == ValuePhase::Commit);
    assert(!std::get<bool>(result.control_edit->event.before));
    assert(std::get<bool>(result.control_edit->event.value));
    assert(!Button(scene, false).control_edit);
    // Without a business response, the next proposal still starts at false.
    Button(scene, true);
    auto repeated = Button(scene, false);
    assert(!std::get<bool>(repeated.control_edit->event.before));
    assert(repeated.control_edit->event.interaction > result.control_edit->event.interaction);

    sdk::ModuleSession module(module_path, "checkbox.fixture", 1,
                              std::bind_front(&Scene::SetBinding, &scene));
    const bool started = module.Start();
    if (!started) {
        std::cerr << module.StartDiagnostic() << " (load_ns=" << module.LoadDurationNs()
                  << ", create_ns=" << module.CreateDurationNs() << ")\n";
    }
    assert(started && module.BackendReady());
    module.ControlValue(*result.control_edit);
    assert(!scene.IsCurrentControlEdit(*result.control_edit));
    assert(scene.Build({1}));
    Button(scene, true);
    auto accepted = Button(scene, false);
    assert(accepted.control_edit && std::get<bool>(accepted.control_edit->event.before));
    assert(!std::get<bool>(accepted.control_edit->event.value));
    assert(accepted.control_edit->event.revision > result.control_edit->event.revision);

    // Value delivery never implicitly calls the legacy on_action path.
    Button(scene, true, {12, 65});
    auto legacy = Button(scene, false, {12, 65});
    assert(legacy.activation && legacy.activation->action == "legacy" && !legacy.control_edit);
    module.StopWork();
    module.ControlValue(*accepted.control_edit);
    assert(!scene.SetBinding("notifications", true)); // Closed module did not write false.
    assert(snapshot->nodes[target.index].enabled);
}

void Cancellations()
{
    Scene scene(ParseBlueprint(source), Shape);
    Prepare(scene);
    const auto target = scene.HitTest({12, 12})->node;
    Button(scene, true);
    assert(!Button(scene, false, {200, 100}).control_edit);

    Button(scene, true);
    assert(scene.SetBinding("canEdit", false));
    assert(!scene.State(target).captured);
    assert(!Button(scene, false).control_edit);
    Button(scene, true); // Old submitted snapshot cannot revive a disabled checkbox.
    assert(!Button(scene, false).control_edit);
    scene.SetBinding("canEdit", true);

    Button(scene, true);
    Key(scene, 0x29, true);
    assert(!Button(scene, false).control_edit);
    Button(scene, true);
    scene.HandleInput(contracts::FocusEvent{{1}, false, {1, 2, 1}});
    assert(!Button(scene, false).control_edit);

    Button(scene, true);
    scene.SetBinding("notifications", true);
    scene.SetBinding("notifications", false); // ABA change still cancels old capture.
    assert(!Button(scene, false).control_edit);
    Button(scene, true);
    assert(scene.Preflight({{"notifications", true}}));
    assert(!Button(scene, false).control_edit);
    Button(scene, true);
    assert(scene.SetBinding("available", false));
    assert(!Button(scene, false).control_edit);
}

void Keyboard()
{
    Scene scene(ParseBlueprint(source), Shape);
    Prepare(scene);
    Key(scene, 0x2b, true);
    Key(scene, 0x28, true);
    assert(!Key(scene, 0x28, false).control_edit); // Checkbox activation uses Space.
    Key(scene, 0x2c, true);
    assert(!Key(scene, 0x2c, true, true).control_edit);
    assert(Key(scene, 0x2c, false).control_edit);
    assert(!Key(scene, 0x2c, false).control_edit);

    Button(scene, true);
    Key(scene, 0x2c, true); // Keyboard takes ownership; old pointer release is inert.
    assert(!Button(scene, false).control_edit);
    assert(Key(scene, 0x2c, false).control_edit);
    Key(scene, 0x2c, true);
    Button(scene, true); // Pointer takes ownership; old key release is inert.
    assert(!Key(scene, 0x2c, false).control_edit);
    assert(Button(scene, false).control_edit);

    Key(scene, 0x2c, true);
    Key(scene, 0x29, true);
    assert(!Key(scene, 0x2c, false).control_edit);
    Key(scene, 0x2c, true);
    scene.SetBinding("notifications", true);
    assert(!Key(scene, 0x2c, false).control_edit);
    Key(scene, 0x2c, true);
    Key(scene, 0x2b, true);
    assert(!Key(scene, 0x2c, false).control_edit); // Focus moved to legacy Toggle.
}

void Schema()
{
    for (const auto text : {"Checkbox(checked: 1)", "Checkbox(checked: \"true\")",
                            "Checkbox.transition(property: \"checked\", durationMs: 100)",
                            "Card { Visual { Checkbox(action: \"x\") } }"}) {
        bool rejected = false;
        try {
            ParseBlueprint(text);
        } catch (const std::exception &) {
            rejected = true;
        }
        assert(rejected);
    }
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2);
    Schema();
    PointerAndBusiness(argv[1]);
    Cancellations();
    Keyboard();
}
