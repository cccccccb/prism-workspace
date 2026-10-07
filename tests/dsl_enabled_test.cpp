#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"
#include <cassert>
#include <stdexcept>

using namespace prism;
using namespace prism::runtime;

namespace {
ShapedText Shape(std::string_view text, double size)
{
    return {{}, text.size() * 7.0, size};
}

InteractionResult Button(Scene &scene, contracts::ButtonState state,
                         const std::shared_ptr<const InputSnapshot> &snapshot)
{
    return scene.HandleInput(
        contracts::PointerButtonEvent{
            {1}, {12, 12}, contracts::PointerButton::Primary, state, 0, 1, {1, 1, 1}},
        snapshot);
}

void Reject(std::string_view source)
{
    bool rejected = false;
    try {
        ParseBlueprint(source);
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);
}
} // namespace

int main()
{
    Scene scene(ParseBlueprint(R"(
        VStack(spacing: 0, enabled: $available) {
            InteractionTarget(height: 40, action: "save", enabled: $canSave) {
                Visual(width: 32, height: 8, background: #80B8FFFF)
                    .state(when: "disabled", scope: "target", opacity: 0.4)
            }
            Button("Cancel", height: 40, action: "cancel")
        }
    )"),
                Shape);
    scene.SetBinding("available", true);
    scene.SetBinding("canSave", true);
    scene.SetViewport({200, 100});
    assert(scene.Build({1}));
    const auto hit = scene.HitTest({12, 12});
    assert(hit);
    const auto save = hit->node;
    auto snapshot = scene.InputGeometry();
    const auto layouts = scene.GetRenderStats().layouts;
    Button(scene, contracts::ButtonState::Pressed, snapshot);
    assert(scene.State(save).captured);

    assert(scene.SetBinding("canSave", false));
    assert(!scene.State(save).enabled && !scene.State(save).captured);
    assert(!Button(scene, contracts::ButtonState::Released, snapshot).activation);
    assert(!Button(scene, contracts::ButtonState::Pressed, snapshot).activation);
    assert(!scene.State(save).captured);
    assert(!scene.HitTest({12, 12}));
    assert(scene.FocusNext() && scene.FocusedAction() == "cancel");
    assert(scene.Build({1}));
    assert(scene.GetRenderStats().layouts == layouts);
    assert(snapshot->nodes[save.index].enabled);
    assert(!scene.InputGeometry()->nodes[save.index].enabled);
    assert(!scene.SetBinding("canSave", false));
    assert(!scene.Build({1}));

    // Parent availability never overwrites the child's local disabled state.
    assert(scene.SetBinding("available", false));
    assert(!scene.FocusedAction());
    assert(!scene.FocusNext());
    assert(scene.SetBinding("available", true));
    assert(!scene.State(save).enabled);
    assert(scene.SetBinding("canSave", true));
    assert(scene.State(save).enabled);
    assert(scene.Build({1}));
    snapshot = scene.InputGeometry();
    assert(!Button(scene, contracts::ButtonState::Released, snapshot).activation);
    Button(scene, contracts::ButtonState::Pressed, snapshot);
    auto released = Button(scene, contracts::ButtonState::Released, snapshot);
    assert(released.activation && released.activation->action == "save");

    // C++ updates and DSL properties survive the Blueprint/theme rebuild path.
    assert(scene.SetEnabled(save, false));
    contracts::ThemeSnapshot theme;
    theme.id = "enabled-test";
    theme.name = "Enabled test";
    theme.generation = 1;
    assert(scene.ApplyTheme(theme));
    assert(!scene.State(save).enabled && !scene.HitTest({12, 12}));
    assert(scene.SetProperty(save, DslProperty::Enabled, true));
    assert(!scene.SetProperty(save, DslProperty::Enabled, 1.0));
    assert(scene.State(save).enabled);

    // Keyboard press cannot survive an atomic binding projection that disables its owner.
    assert(scene.FocusNext());
    const auto focused = scene.FocusedAction();
    assert(focused.has_value());
    const contracts::KeyEvent key{{1},       0x2c, contracts::ButtonState::Pressed, false, 1,
                                  {1, 2, 1}, {}};
    scene.HandleInput(key);
    assert(scene.Preflight({{"available", false}}));
    assert(!scene.FocusedAction());
    auto key_up = key;
    key_up.state = contracts::ButtonState::Released;
    assert(!scene.HandleInput(key_up).activation);
    assert(!scene.State(save).enabled);
    assert(scene.Preflight({{"available", true}, {"canSave", false}}));
    assert(!scene.State(save).enabled);
    assert(scene.FocusNext() && scene.FocusedAction() == "cancel");

    Scene disabled(ParseBlueprint(R"(
        VStack(enabled: false) {
            Button("Save", enabled: true, action: "save", height: 40)
        }
    )"),
                   Shape);
    disabled.SetViewport({200, 100});
    assert(disabled.Build({1}));
    assert(!disabled.FocusNext() && !disabled.HitTest({12, 12}));

    Reject("Button(\"Save\", enabled: 1)");
    Reject("Button(\"Save\", enabled: \"false\")");
    Reject(
        R"(InteractionTarget { Visual.state(when: "disabled", scope: "target", enabled: true) })");
    Reject("Button(\"Save\").transition(property: \"enabled\", durationMs: 100)");
}
