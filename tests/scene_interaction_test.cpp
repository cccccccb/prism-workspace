#include "prism/contracts/events.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"

#include <cassert>
#include <limits>
#include <span>
#include <string>
#include <string_view>

using namespace prism;
using namespace prism::runtime;

namespace {
constexpr contracts::WindowId window{1};
constexpr contracts::InputSource pointer{1, 1, 1};
constexpr contracts::InputSource otherPointer{1, 2, 1};
constexpr contracts::InputSource recreatedPointer{1, 1, 2};
constexpr contracts::InputSource secondSeatPointer{2, 1, 1};
constexpr contracts::InputSource keyboard{1, 3, 1};
constexpr contracts::InputSource otherKeyboard{2, 3, 1};

ShapedText Shape(std::string_view text, double size)
{
    return {{}, text.size() * 7.0, size};
}

void Layout(Scene &scene, contracts::LogicalSize size = {180, 60})
{
    assert(scene.SetViewport(size));
    assert(scene.Build(window));
    scene.AcknowledgeComposite();
}

void Commit(Scene &scene)
{
    assert(scene.Build(window));
    scene.AcknowledgeComposite();
}

contracts::NodeId Target(const Scene &scene, contracts::LogicalPoint position)
{
    const auto hit = scene.HitTest(position);
    assert(hit);
    return hit->node;
}

InteractionResult Move(Scene &scene, contracts::LogicalPoint position,
                       contracts::InputSource source = pointer)
{
    return scene.HandleInput(contracts::PointerMotionEvent{window, position, 1, source});
}

InteractionResult Button(Scene &scene, contracts::LogicalPoint position,
                         contracts::ButtonState state, contracts::InputSource source = pointer,
                         contracts::PointerButton button = contracts::PointerButton::Primary)
{
    return scene.HandleInput(
        contracts::PointerButtonEvent{window, position, button, state, 0, 1, source});
}

InteractionResult Down(Scene &scene, contracts::LogicalPoint position,
                       contracts::InputSource source = pointer)
{
    return Button(scene, position, contracts::ButtonState::Pressed, source);
}

InteractionResult Up(Scene &scene, contracts::LogicalPoint position,
                     contracts::InputSource source = pointer)
{
    return Button(scene, position, contracts::ButtonState::Released, source);
}

InteractionResult Key(Scene &scene, std::uint32_t code, contracts::ButtonState state,
                      bool repeat = false, bool shift = false,
                      contracts::InputSource source = keyboard)
{
    return scene.HandleInput(contracts::KeyEvent{window, code, state, repeat, 1, source, {shift}});
}

void ExpectActivation(const InteractionResult &result, contracts::NodeId node,
                      std::string_view action)
{
    assert(result.activation);
    assert(result.activation->node == node);
    assert(result.activation->action == action);
}

void CheckDuplicateActions()
{
    Scene scene(ParseBlueprint(R"(
        HStack(spacing:20) {
            IconButton("play","shared",width:50,height:40)
            IconButton("play","shared",width:50,height:40)
        }
    )"),
                Shape);
    Layout(scene);
    const auto first = Target(scene, {20, 20});
    const auto second = Target(scene, {90, 20});
    assert(first != second);
    assert(scene.HitTest({90, 20})->localPosition == (contracts::LogicalPoint{20, 20}));

    assert(Move(scene, {20, 20}).changed);
    assert(scene.State(first).hovered && !scene.State(second).hovered);
    assert(Move(scene, {90, 20}).changed);
    assert(!scene.State(first).hovered && scene.State(second).hovered);
    Commit(scene);
    const auto pixels = scene.PixelsRevision();
    assert(!Move(scene, {91, 20}).changed);
    assert(scene.PixelsRevision() == pixels); // movement within one target stays idle

    const auto press = Down(scene, {90, 20});
    assert(press.changed && !press.activation);
    assert(scene.State(second).pressed && scene.State(second).captured);
    assert(!scene.State(first).pressed && !scene.State(first).captured);
    ExpectActivation(Up(scene, {90, 20}), second, "shared");
    assert(!scene.State(second).pressed && !scene.State(second).captured);
    assert(!Up(scene, {90, 20}).activation);

    assert(!Down(scene, {20, 20}).activation);
    assert(!Up(scene, {90, 20}).activation); // matching action is not matching identity
    assert(!scene.State(first).captured);
    assert(!scene.State({999, 1}).enabled);
}

void CheckHitGeometry()
{
    Scene rounded(ParseBlueprint(R"(
        Card(clip:true,cornerRadius:20) {
            IconButton("play","rounded",width:80,height:40)
        }
    )"),
                  Shape);
    Layout(rounded, {80, 40});
    assert(!rounded.HitTest({0, 0}));
    assert(rounded.HitTest({20, 10}));
    assert(!rounded.HitTest({80, 20}));
    assert(!rounded.HitTest({20, 40}));
    assert(!rounded.HitTest({std::numeric_limits<double>::quiet_NaN(), 10}));

    Scene clipped(ParseBlueprint(R"(
        Card(clip:true) {
            HStack(width:60,height:30,overflow:"clip") {
                IconButton("play","overflow",width:120,height:30)
            }
        }
    )"),
                  Shape);
    Layout(clipped, {160, 50});
    assert(clipped.HitTest({59, 10}));
    assert(!clipped.HitTest({61, 10}));

    Scene overlap(ParseBlueprint(R"(
        Card {
            IconButton("play","back",width:80,height:40)
            IconButton("play","front",width:40,height:40)
        }
    )"),
                  Shape);
    Layout(overlap);
    const auto front = Target(overlap, {20, 20});
    const auto back = Target(overlap, {60, 20});
    assert(front != back);
    Down(overlap, {20, 20});
    ExpectActivation(Up(overlap, {20, 20}), front, "front");
    assert(overlap.SetProperty(front, DslProperty::Visible, false));
    Commit(overlap);
    assert(Target(overlap, {20, 20}) == back);
}

void CheckPointerLifecycle()
{
    Scene scene(ParseBlueprint("IconButton(\"play\",\"go\",width:50,height:40)"), Shape);
    Layout(scene, {50, 40});
    const auto node = Target(scene, {20, 20});
    assert(!Up(scene, {20, 20}).activation);
    assert(!Button(scene, {20, 20}, contracts::ButtonState::Pressed, pointer,
                   contracts::PointerButton::Secondary)
                .activation);
    assert(!scene.State(node).captured);

    Down(scene, {20, 20});
    assert(scene.HandleInput(contracts::PointerLeaveEvent{window, 2, pointer}).changed);
    assert(!scene.State(node).hovered && scene.State(node).captured);
    assert(!Up(scene, {100, 50}).activation);
    assert(!scene.State(node).captured && !scene.State(node).pressed);

    scene.HandleInput(contracts::PointerEnterEvent{window, {20, 20}, 3, pointer});
    Down(scene, {20, 20});
    assert(scene.State(node).captured);
    scene.HandleInput(contracts::PointerLeaveEvent{window, 3, pointer});
    assert(!Up(scene, {20, 20}).activation); // Wayland buttons can carry the last cached position
    assert(!scene.State(node).hovered && !scene.State(node).captured);

    scene.HandleInput(contracts::PointerEnterEvent{window, {20, 20}, 4, pointer});
    Down(scene, {20, 20});
    scene.HandleInput(contracts::PointerLeaveEvent{window, 3, pointer});
    scene.HandleInput(contracts::PointerEnterEvent{window, {20, 20}, 4, pointer});
    assert(scene.State(node).hovered && scene.State(node).captured);
    ExpectActivation(Up(scene, {20, 20}), node, "go");

    Down(scene, {20, 20});
    scene.HandleInput(contracts::PointerLeaveEvent{window, 5, pointer});
    Move(scene, {20, 20});
    assert(scene.State(node).hovered && scene.State(node).captured);
    ExpectActivation(Up(scene, {20, 20}), node, "go");

    Down(scene, {20, 20});
    Move(scene, {100, 50});
    assert(scene.State(node).captured && !scene.State(node).pressed);
    Move(scene, {20, 20});
    assert(scene.State(node).captured && scene.State(node).pressed);
    ExpectActivation(Up(scene, {20, 20}), node, "go");

    Down(scene, {20, 20});
    assert(scene.HandleInput(contracts::PointerCancelEvent{window, 5, pointer}).changed);
    assert(!scene.State(node).hovered && !scene.State(node).pressed && !scene.State(node).captured);
    assert(!Up(scene, {20, 20}).activation);

    Down(scene, {20, 20});
    assert(scene.CancelInput());
    assert(!scene.State(node).captured && !scene.State(node).pressed);
    assert(!Up(scene, {20, 20}).activation);
}

void CheckSourceOwnership()
{
    Scene scene(ParseBlueprint("IconButton(\"play\",\"go\",width:50,height:40)"), Shape);
    Layout(scene);
    const auto node = Target(scene, {20, 20});
    Down(scene, {20, 20});
    assert(!Up(scene, {20, 20}, otherPointer).activation);
    assert(!Up(scene, {20, 20}, recreatedPointer).activation);
    scene.HandleInput(contracts::PointerCancelEvent{window, 2, otherPointer});
    assert(scene.State(node).captured && scene.State(node).pressed);
    ExpectActivation(Up(scene, {20, 20}), node, "go");

    // Removing one source must not cancel the current source with a newer generation.
    Down(scene, {20, 20}, recreatedPointer);
    scene.HandleInput(contracts::PointerCancelEvent{window, 3, pointer});
    assert(scene.State(node).captured);
    ExpectActivation(Up(scene, {20, 20}, recreatedPointer), node, "go");
}

void CheckChangedTargets()
{
    Scene scene(ParseBlueprint(R"(
        Card(visible:$shown) {
            IconButton("play",$action,width:50,height:40)
        }
    )"),
                Shape);
    assert(scene.SetBinding("action", std::string("before")));
    Layout(scene);
    const auto node = Target(scene, {20, 20});
    Down(scene, {20, 20});
    assert(scene.SetBinding("action", std::string("after")));
    assert(!scene.State(node).captured && !scene.State(node).pressed);
    assert(!Up(scene, {20, 20}).activation);
    Down(scene, {20, 20});
    ExpectActivation(Up(scene, {20, 20}), node, "after");

    Down(scene, {20, 20});
    assert(scene.SetBinding("shown", false));
    assert(!scene.State(node).captured && !scene.State(node).hovered);
    assert(scene.SetBinding("shown", true));
    Commit(scene);
    assert(!Up(scene, {20, 20}).activation); // hiding and revealing cannot revive a press

    Down(scene, {20, 20});
    assert(scene.SetEnabled(node, false));
    assert(!scene.State(node).enabled && !scene.State(node).captured);
    assert(!scene.HitTest({20, 20}));
    assert(scene.SetEnabled(node, true));
    assert(!Up(scene, {20, 20}).activation);
    Down(scene, {20, 20});
    ExpectActivation(Up(scene, {20, 20}), node, "after");

    Down(scene, {20, 20});
    assert(scene.SetEnabled(scene.RootId(), false));
    assert(!scene.State(node).enabled && !scene.State(node).captured && !scene.State(node).focused);
    assert(!scene.HitTest({20, 20}));
    assert(scene.SetEnabled(scene.RootId(), true));
    assert(scene.State(node).enabled);
    assert(!Up(scene, {20, 20}).activation);
}

void CheckSeatFocusLoss()
{
    Scene scene(ParseBlueprint(R"(
        HStack(spacing:20) {
            IconButton("play","one",width:50,height:40)
            IconButton("play","two",width:50,height:40)
        }
    )"),
                Shape);
    Layout(scene);
    const auto first = Target(scene, {20, 20});
    const auto second = Target(scene, {90, 20});
    Down(scene, {20, 20});
    Down(scene, {90, 20}, secondSeatPointer);
    assert(scene.State(first).captured && scene.State(second).captured);

    scene.HandleInput(contracts::FocusEvent{window, false, keyboard});
    assert(!scene.State(first).captured && !scene.State(first).focused);
    assert(scene.State(second).captured && scene.State(second).focused);
    assert(!Up(scene, {20, 20}).activation);
    ExpectActivation(Up(scene, {90, 20}, secondSeatPointer), second, "two");
}

void CheckGeometryChanges()
{
    Scene scene(ParseBlueprint(R"(
        HStack(spacing:20) {
            IconButton("play","one",width:50,height:40)
            IconButton("play","two",width:50,height:40)
        }
    )"),
                Shape);
    Layout(scene);
    const auto first = Target(scene, {20, 20});
    const auto second = Target(scene, {90, 20});
    Move(scene, {90, 20});
    assert(scene.State(second).hovered);
    assert(scene.SetProperty(first, DslProperty::Width, 100.0));
    Commit(scene);
    assert(scene.State(first).hovered && !scene.State(second).hovered);

    Down(scene, {90, 20});
    assert(scene.SetProperty(first, DslProperty::Width, 40.0));
    Commit(scene);
    assert(scene.State(first).captured && !scene.State(first).pressed);
    assert(scene.State(second).hovered);
    assert(!Up(scene, {90, 20}).activation);

    Scene available(ParseBlueprint(R"(
        Card { IconButton("play","",width:50,height:40) }
    )"),
                    Shape);
    Layout(available);
    Move(available, {20, 20});
    assert(!available.HitTest({20, 20}));
    const contracts::NodeId child{1, 1};
    assert(available.SetProperty(child, DslProperty::Action, std::string("new")));
    assert(available.State(child).hovered);
    Down(available, {20, 20});
    ExpectActivation(Up(available, {20, 20}), child, "new");
}

Blueprint Action(std::string action)
{
    Blueprint result;
    result.properties = {{DslProperty::Width, 50.0},
                         {DslProperty::Height, 40.0},
                         {DslProperty::Action, std::move(action)}};
    return result;
}

void CheckRegionLifetime()
{
    Blueprint region;
    region.region = "body";
    region.properties = {{DslProperty::Width, 50.0}, {DslProperty::Height, 40.0}};
    region.children.push_back(Action("placeholder"));
    Blueprint root;
    root.kind = Kind::Row;
    root.properties = {{DslProperty::Spacing, 20.0}};
    root.children = {Action("retained"), region};
    Scene scene(root, Shape);
    Layout(scene);
    const auto removed = Target(scene, {90, 20});
    Down(scene, {90, 20});
    const RegionUpdate update{"body", Action("mounted")};
    assert(scene.MountRegions(std::span(&update, 1), {}));
    assert(!scene.State(removed).enabled && !scene.State(removed).captured);
    Commit(scene);
    const auto mounted = Target(scene, {90, 20});
    assert(mounted != removed);
    assert(!Up(scene, {90, 20}).activation);
    Down(scene, {90, 20});
    ExpectActivation(Up(scene, {90, 20}), mounted, "mounted");

    Scene retained(root, Shape);
    Layout(retained);
    const auto stable = Target(retained, {20, 20});
    Down(retained, {20, 20});
    assert(retained.MountRegions(std::span(&update, 1), {}));
    Commit(retained);
    assert(Target(retained, {20, 20}) == stable && retained.State(stable).captured);
    ExpectActivation(Up(retained, {20, 20}), stable, "retained");

    Scene disabled(root, Shape);
    Layout(disabled);
    assert(disabled.SetEnabled(disabled.RegionId("body"), false));
    assert(disabled.MountRegions(std::span(&update, 1), {}));
    Commit(disabled);
    assert(!disabled.HitTest({90, 20}));
    assert(disabled.SetEnabled(disabled.RegionId("body"), true));
    const auto enabledChild = Target(disabled, {90, 20});
    assert(disabled.State(enabledChild).enabled);
}

void CheckKeyboard()
{
    Scene scene(ParseBlueprint(R"(
        HStack(spacing:20) {
            IconButton("play","one",width:50,height:40)
            IconButton("play","two",width:50,height:40)
        }
    )"),
                Shape);
    Layout(scene);
    const auto first = Target(scene, {20, 20});
    const auto second = Target(scene, {90, 20});
    scene.HandleInput(contracts::FocusEvent{window, true, keyboard});
    assert(Key(scene, 0x2b, contracts::ButtonState::Pressed).changed);
    assert(scene.State(first).focused && scene.State(first).focusVisible);
    Key(scene, 0x2b, contracts::ButtonState::Pressed, true);
    assert(scene.State(first).focused); // platform repeat is not another navigation press
    Key(scene, 0x2b, contracts::ButtonState::Released);
    Key(scene, 0x2b, contracts::ButtonState::Pressed, false, true);
    Key(scene, 0x2b, contracts::ButtonState::Released, false, true);
    assert(scene.State(second).focused && !scene.State(first).focused);

    assert(!Key(scene, 0x28, contracts::ButtonState::Released).activation);
    assert(!Key(scene, 0x28, contracts::ButtonState::Pressed).activation);
    assert(scene.State(second).pressed);
    assert(!Key(scene, 0x28, contracts::ButtonState::Pressed, true).activation);
    assert(!Key(scene, 0x28, contracts::ButtonState::Released, false, false, otherKeyboard)
                .activation);
    ExpectActivation(Key(scene, 0x28, contracts::ButtonState::Released), second, "two");
    assert(!scene.State(second).pressed);
    assert(!Key(scene, 0x28, contracts::ButtonState::Released).activation);

    Key(scene, 0x28, contracts::ButtonState::Pressed);
    Key(scene, 0x2b, contracts::ButtonState::Pressed);
    Key(scene, 0x2b, contracts::ButtonState::Released);
    assert(scene.State(first).focused && !scene.State(second).pressed);
    assert(!Key(scene, 0x28, contracts::ButtonState::Released).activation);
    Key(scene, 0x2b, contracts::ButtonState::Pressed);
    Key(scene, 0x2b, contracts::ButtonState::Released);
    assert(scene.State(second).focused);

    Key(scene, 0x2c, contracts::ButtonState::Pressed);
    assert(scene.State(second).pressed);
    Key(scene, 0x29, contracts::ButtonState::Pressed);
    assert(!Key(scene, 0x2c, contracts::ButtonState::Released).activation);

    Key(scene, 0x28, contracts::ButtonState::Pressed);
    scene.HandleInput(contracts::FocusEvent{window, false, keyboard});
    assert(!scene.State(second).focused && !scene.State(second).pressed);
    assert(!Key(scene, 0x28, contracts::ButtonState::Released).activation);

    scene.HandleInput(contracts::FocusEvent{window, true, keyboard});
    assert(scene.SetEnabled(first, false));
    Key(scene, 0x2b, contracts::ButtonState::Pressed);
    Key(scene, 0x2b, contracts::ButtonState::Released);
    assert(scene.State(second).focused);
    Key(scene, 0x2c, contracts::ButtonState::Pressed);
    ExpectActivation(Key(scene, 0x2c, contracts::ButtonState::Released), second, "two");
}
} // namespace

int main()
{
    CheckDuplicateActions();
    CheckHitGeometry();
    CheckPointerLifecycle();
    CheckSourceOwnership();
    CheckChangedTargets();
    CheckSeatFocusLoss();
    CheckGeometryChanges();
    CheckRegionLifetime();
    CheckKeyboard();
}
