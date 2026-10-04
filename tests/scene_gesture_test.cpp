#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"

#include <cassert>
#include <limits>
#include <string>
#include <string_view>

using namespace prism;
using namespace prism::runtime;
using contracts::GesturePhase;

namespace {
constexpr contracts::WindowId window{1};
constexpr contracts::InputSource pointer{1, 1, 1};
constexpr contracts::InputSource touch{1, 2, 1};
constexpr contracts::InputSource otherTouch{1, 3, 1};
constexpr contracts::InputSource keyboard{1, 4, 1};

ShapedText Shape(std::string_view text, double size)
{
    return {{}, text.size() * 7.0, size};
}

Blueprint Ui()
{
    return ParseBlueprint(R"(
        Card {
            InteractionTarget(action:"click",width:80,height:40) {
                Visual { Card(background:#123456ff) }
                    .state(when:"dragging",scope:"target",opacity:0.5)
            }.gesture(action:"resize",threshold:6)
        }
    )");
}

void Layout(Scene &scene)
{
    assert(scene.SetViewport({180, 60}));
    assert(scene.Build(window));
    scene.AcknowledgeComposite();
}

InteractionResult Button(Scene &scene, bool down, contracts::LogicalPoint point = {10, 10},
                         const std::shared_ptr<const InputSnapshot> &snapshot = {})
{
    contracts::PointerButtonEvent event{window,
                                        point,
                                        contracts::PointerButton::Primary,
                                        down ? contracts::ButtonState::Pressed
                                             : contracts::ButtonState::Released,
                                        0,
                                        down ? 1U : 9U,
                                        pointer,
                                        down ? 731U : 900U};
    return snapshot ? scene.HandleInput(event, snapshot) : scene.HandleInput(event);
}

void Move(Scene &scene, contracts::LogicalPoint point, std::uint64_t time = 2)
{
    scene.HandleInput(contracts::PointerMotionEvent{window, point, time, pointer});
}

contracts::GestureEvent Only(Scene &scene, GesturePhase phase)
{
    auto events = scene.TakeGestureEvents();
    assert(events.size() == 1 && events[0].phase == phase);
    return std::move(events[0]);
}

void CheckPointer()
{
    Scene scene(Ui(), Shape);
    Layout(scene);
    const auto target = scene.HitTest({10, 10})->node;
    Button(scene, true);
    assert(scene.State(target).captured && !scene.State(target).dragging);
    assert(scene.TakeGestureEvents().empty());
    Move(scene, {13, 14}); // Five logical pixels: ordinary button semantics.
    assert(!scene.State(target).dragging);
    assert(scene.TakeGestureEvents().empty());
    assert(Button(scene, false, {13, 14}).activation->action == "click");
    assert(scene.TakeGestureEvents().empty());

    Button(scene, true);
    Move(scene, {16, 10}); // Exact Euclidean threshold.
    const auto begin = Only(scene, GesturePhase::Begin);
    assert(begin.id && begin.node == target && begin.action == "resize");
    assert(begin.source == pointer && !begin.touch && begin.serial == 731);
    assert((begin.start == contracts::LogicalPoint{10, 10}));
    assert((begin.position == contracts::LogicalPoint{16, 10}));
    assert(scene.State(target).dragging);
    Move(scene, {100, 80}, 4); // Outside the window: retained capture still moves.
    const auto update = Only(scene, GesturePhase::Update);
    assert(update.id == begin.id && update.serial == 731 && update.time_ns == 4);
    assert((update.position == contracts::LogicalPoint{100, 80}));
    assert(!scene.State(target).pressed && scene.State(target).dragging);
    assert(!Button(scene, false, {100, 80}).activation);
    const auto end = Only(scene, GesturePhase::End);
    assert(end.id == begin.id && end.position == update.position && end.time_ns == 9);
    assert(!scene.State(target).captured && !scene.State(target).dragging);
    assert(!Button(scene, false).activation);
    assert(scene.TakeGestureEvents().empty());

    Button(scene, true);
    Move(scene, {16, 10});
    const auto second = Only(scene, GesturePhase::Begin);
    assert(second.id > begin.id);
    assert(!Button(scene, false, {16, 10}).activation); // Release inside still suppresses click.
    Only(scene, GesturePhase::End);
}

void CheckImmediateGesture()
{
    Scene scene(ParseBlueprint(R"(InteractionTarget(width:80,height:40,action:"click")
                                   {}.gesture(action:"window",threshold:0))"),
                Shape);
    Layout(scene);
    Button(scene, true);
    const auto begin = Only(scene, GesturePhase::Begin);
    assert(begin.serial == 731 && begin.time_ns == 1);
    assert(!Button(scene, false).activation);
    assert(Only(scene, GesturePhase::End).id == begin.id);

    Button(scene, true);
    Button(scene, false);
    const auto queued = scene.TakeGestureEvents();
    assert(queued.size() == 2 && queued[0].phase == GesturePhase::Begin &&
           queued[1].phase == GesturePhase::End && queued[0].id == queued[1].id);
}

void CheckCancellation()
{
    for (int reason = 0; reason < 9; ++reason) {
        Scene scene(Ui(), Shape);
        Layout(scene);
        const auto target = scene.HitTest({10, 10})->node;
        Button(scene, true);
        Move(scene, {16, 10});
        const auto begin = Only(scene, GesturePhase::Begin);
        switch (reason) {
        case 0:
            assert(scene.SetEnabled(target, false));
            break;
        case 1:
            assert(scene.SetProperty(scene.RootId(), DslProperty::Visible, false));
            break;
        case 2:
            assert(scene.SetProperty(target, DslProperty::Action, std::string("other")));
            break;
        case 3:
            assert(scene.SetGesture(target, GestureSpec{"other", 6}));
            break;
        case 4:
            scene.HandleInput(contracts::PointerCancelEvent{window, 5, pointer});
            break;
        case 5:
            scene.HandleInput(contracts::FocusEvent{window, false, keyboard});
            break;
        case 6:
            scene.HandleInput(contracts::KeyEvent{window, 0x29, contracts::ButtonState::Pressed,
                                                  false, 5, keyboard});
            break;
        case 7:
            scene.HandleInput(contracts::CloseRequestedEvent{window});
            break;
        case 8:
            scene.CancelInput();
            break;
        }
        const auto cancel = Only(scene, GesturePhase::Cancel);
        assert(cancel.id == begin.id && cancel.action == "resize");
        if (reason == 4 || reason == 6) {
            assert(cancel.time_ns == 5);
        }
        assert(!scene.State(target).dragging && !scene.State(target).captured);
        Move(scene, {20, 10});
        assert(!Button(scene, false).activation);
        scene.CancelInput();
        assert(scene.TakeGestureEvents().empty());
    }
}

void CheckTouch()
{
    Scene scene(Ui(), Shape);
    Layout(scene);
    scene.HandleInput(contracts::TouchDownEvent{window, {10, 10}, 0, 1, touch, 72});
    scene.HandleInput(contracts::TouchDownEvent{window, {20, 10}, 1, 1, touch, 73});
    scene.HandleInput(contracts::TouchDownEvent{window, {30, 10}, 0, 1, otherTouch, 74});
    scene.HandleInput(contracts::TouchMotionEvent{window, {17, 10}, 0, 2, touch});
    const auto one = Only(scene, GesturePhase::Begin);
    scene.HandleInput(contracts::TouchMotionEvent{window, {27, 10}, 1, 2, touch});
    const auto two = Only(scene, GesturePhase::Begin);
    scene.HandleInput(contracts::TouchMotionEvent{window, {37, 10}, 0, 2, otherTouch});
    const auto other = Only(scene, GesturePhase::Begin);
    assert(one.touch && one.contact == 0 && one.serial == 72 && one.source == touch);
    assert(two.touch && two.contact == 1 && two.serial == 73 && two.id != one.id);
    assert(other.contact == 0 && other.source == otherTouch && other.id != one.id);
    scene.HandleInput(contracts::TouchCancelEvent{window, 4, touch});
    const auto canceled = scene.TakeGestureEvents();
    assert(canceled.size() == 2);
    assert(canceled[0].phase == GesturePhase::Cancel && canceled[1].phase == GesturePhase::Cancel);
    assert(canceled[0].time_ns == 4 && canceled[1].time_ns == 4);
    scene.HandleInput(contracts::TouchMotionEvent{window, {-20, 80}, 0, 5, otherTouch});
    const auto moved = Only(scene, GesturePhase::Update);
    assert(moved.id == other.id && (moved.position == contracts::LogicalPoint{-20, 80}));
    assert(!scene.HandleInput(contracts::TouchUpEvent{window, 0, 6, otherTouch}).activation);
    assert(Only(scene, GesturePhase::End).id == other.id);
    assert(!scene.HandleInput(contracts::TouchUpEvent{window, 0, 6, touch}).activation);
    assert(scene.TakeGestureEvents().empty());
    scene.HandleInput(contracts::TouchDownEvent{window, {10, 10}, 0, 7, touch, 80});
    assert(scene.HandleInput(contracts::TouchUpEvent{window, 0, 8, touch}).activation);
    assert(scene.TakeGestureEvents().empty());
}

void CheckSnapshotAndFinite()
{
    Scene scene(Ui(), Shape);
    Layout(scene);
    const auto old = scene.InputGeometry();
    const auto target = scene.HitTest({10, 10})->node;
    assert(scene.SetProperty(scene.RootId(), DslProperty::Padding, 35.0));
    scene.Build(window);
    Button(scene, true, {10, 10}, old); // Old submitted target, current layout moved away.
    scene.HandleInput(contracts::PointerMotionEvent{window, {17, 10}, 2, pointer}, old);
    const auto begin = Only(scene, GesturePhase::Begin);
    assert(begin.node == target && begin.snapshot_scene == old->scene &&
           begin.snapshot_version == old->version);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    Move(scene, {nan, 10});
    Move(scene, {10, std::numeric_limits<double>::infinity()});
    assert(scene.TakeGestureEvents().empty() && scene.State(target).dragging);
    assert(!Button(scene, false, {17, 10}, old).activation);
    const auto end = Only(scene, GesturePhase::End);
    assert((end.position == contracts::LogicalPoint{17, 10}));

    assert(scene.SetGesture(target, GestureSpec{"changed", 6}));
    Button(scene, true, {10, 10}, old); // Old gesture declaration cannot create a new stream.
    scene.HandleInput(contracts::PointerMotionEvent{window, {17, 10}, 2, pointer}, old);
    assert(scene.TakeGestureEvents().empty());
    assert(!Button(scene, false, {17, 10}, old).activation);
}

void CheckUnmount()
{
    auto root = ParseBlueprint("Card {}");
    auto region = ParseBlueprint("Card {}");
    region.region = "body";
    region.children.push_back(Ui());
    root.children.push_back(std::move(region));
    Scene scene(std::move(root), Shape);
    Layout(scene);
    Button(scene, true);
    Move(scene, {17, 10});
    const auto begin = Only(scene, GesturePhase::Begin);
    const RegionUpdate update{"body", ParseBlueprint("Card {}")};
    assert(scene.MountRegions(std::span(&update, 1), {}));
    assert(Only(scene, GesturePhase::Cancel).id == begin.id);
    assert(!Button(scene, false).activation);
    assert(scene.TakeGestureEvents().empty());
}

void Reject(std::string_view source)
{
    bool failed = false;
    try {
        (void)PrepareComponent(source);
    } catch (const LoadFailure &) {
        failed = true;
    }
    assert(failed);
}

void CheckDsl()
{
    const auto prepared = PrepareComponent("InteractionTarget {}.gesture(action:\"move\")");
    assert(prepared.Root().gesture && prepared.Root().gesture->threshold == 6);
    const auto linked = LinkComponent(prepared);
    assert(linked.gesture == prepared.Root().gesture);
    Reject("Card {}.gesture(action:\"move\")");
    Reject("InteractionTarget {}.gesture(action:\"\")");
    Reject("InteractionTarget {}.gesture(threshold:6)");
    Reject("InteractionTarget {}.gesture(action:\"move\",threshold:-1)");
    Reject("InteractionTarget {}.gesture(action:\"move\",threshold:1025)");
    Reject("InteractionTarget {}.gesture(action:\"move\",threshold:\"6\")");
    Reject("InteractionTarget {}.gesture(action:\"move\",other:6)");
    Reject("InteractionTarget {}.gesture(action:\"move\",action:\"again\")");
    Reject("InteractionTarget {}.gesture(action:\"move\").gesture(action:\"again\")");
    Reject("Card { Visual { InteractionTarget {}.gesture(action:\"move\") } }");

    auto invalid = Ui();
    invalid.gesture = GestureSpec{"move", 6};
    bool failed = false;
    try {
        Scene scene(std::move(invalid), Shape);
    } catch (const std::invalid_argument &) {
        failed = true;
    }
    assert(failed); // Typed Blueprint construction has the same boundary as DSL.
}

void CheckPendingAndIdentity()
{
    Scene scene(Ui(), Shape);
    Layout(scene);
    Button(scene, true);
    Move(scene, {17, 10}, 2);
    Move(scene, {30, 10}, 3);
    Move(scene, {40, 10}, 4);
    Button(scene, false, {40, 10});
    const auto events = scene.TakeGestureEvents();
    assert(events.size() == 3);
    assert(events[0].phase == GesturePhase::Begin && events[0].time_ns == 2);
    assert((events[0].position == contracts::LogicalPoint{17, 10}));
    assert(events[1].phase == GesturePhase::Update);
    assert(events[1].time_ns == 4);
    assert((events[1].position == contracts::LogicalPoint{40, 10}));
    assert(events[2].phase == GesturePhase::End);
    assert(events[2].time_ns == 9);

    Scene replacement(Ui(), Shape);
    Layout(replacement);
    Button(replacement, true);
    Move(replacement, {17, 10});
    assert(Only(replacement, GesturePhase::Begin).id > events[0].id);
}
} // namespace

int main()
{
    CheckDsl();
    CheckPointer();
    CheckImmediateGesture();
    CheckCancellation();
    CheckTouch();
    CheckSnapshotAndFinite();
    CheckUnmount();
    CheckPendingAndIdentity();
}
