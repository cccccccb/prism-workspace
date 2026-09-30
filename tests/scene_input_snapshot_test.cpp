#include "prism/animation/timeline.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"

#include <cassert>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace prism;
using namespace prism::runtime;

namespace {
constexpr contracts::WindowId window{1};
constexpr contracts::InputSource pointer{1, 1, 1};
constexpr contracts::InputSource touch{1, 3, 1};
constexpr contracts::InputSource otherTouch{1, 4, 1};
constexpr contracts::InputSource recreatedTouch{1, 3, 2};
constexpr contracts::InputSource otherSeat{2, 3, 1};
constexpr contracts::InputSource keyboard{1, 2, 1};

class FakeClock final : public animation::AnimationClock {
public:
    std::uint64_t NowNs() const noexcept override
    {
        return now;
    }

    std::uint64_t now{1'000'000'000};
};

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

std::shared_ptr<const InputSnapshot> Build(Scene &scene)
{
    scene.Build(window);
    scene.AcknowledgeComposite();
    assert(scene.InputGeometry());
    return scene.InputGeometry();
}

InteractionResult Button(Scene &scene, const std::shared_ptr<const InputSnapshot> &snapshot,
                         contracts::LogicalPoint position, contracts::ButtonState state)
{
    return scene.HandleInput(contracts::PointerButtonEvent{window, position,
                                                           contracts::PointerButton::Primary, state,
                                                           0, 1, pointer},
                             snapshot);
}

InteractionResult Down(Scene &scene, const std::shared_ptr<const InputSnapshot> &snapshot,
                       contracts::LogicalPoint position)
{
    return Button(scene, snapshot, position, contracts::ButtonState::Pressed);
}

InteractionResult Up(Scene &scene, const std::shared_ptr<const InputSnapshot> &snapshot,
                     contracts::LogicalPoint position)
{
    return Button(scene, snapshot, position, contracts::ButtonState::Released);
}

void TouchDown(Scene &scene, const std::shared_ptr<const InputSnapshot> &snapshot,
               contracts::LogicalPoint point, contracts::InputContactId contact = 0,
               contracts::InputSource source = touch)
{
    scene.HandleInput(contracts::TouchDownEvent{window, point, contact, 1, source}, snapshot);
}

InteractionResult TouchUp(Scene &scene, const std::shared_ptr<const InputSnapshot> &snapshot,
                          contracts::InputContactId contact = 0,
                          contracts::InputSource source = touch)
{
    return scene.HandleInput(contracts::TouchUpEvent{window, contact, 1, source}, snapshot);
}

void ExpectActivation(const InteractionResult &result, contracts::NodeId node,
                      std::string_view action)
{
    assert(result.activation && result.activation->node == node &&
           result.activation->action == action);
}

Blueprint Buttons()
{
    return ParseBlueprint(R"(
        HStack(spacing:20) {
            IconButton("play","one",width:50,height:40)
            IconButton("play","two",width:50,height:40)
        }
    )");
}

void ExpectCaptureNeedsLayout(Scene &scene)
{
    const auto previous = scene.InputGeometry();
    const auto stats = scene.GetRenderStats();
    const auto pixels = scene.PixelsRevision();
    const auto dirty = scene.PendingDirty();
    bool rejected = false;
    try {
        (void)scene.CaptureInputSnapshot();
    } catch (const std::logic_error &) {
        rejected = true;
    }
    assert(rejected);
    assert(scene.InputGeometry() == previous);
    assert(scene.GetRenderStats() == stats);
    assert(scene.PixelsRevision() == pixels);
    assert(scene.PendingDirty() == dirty);
}

void CheckCaptureWithoutBuild()
{
    Scene scene(ParseBlueprint("Card { IconButton(\"play\",\"before\",width:50,height:40) }"),
                Shape);
    ExpectCaptureNeedsLayout(scene); // No viewport or resolved layout exists yet.
    assert(scene.SetViewport({180, 60}));
    ExpectCaptureNeedsLayout(scene); // A viewport alone does not resolve node bounds.
    assert(scene.Build(window));
    scene.AcknowledgeComposite();
    const auto original = scene.InputGeometry();
    const auto target = scene.HitTest({20, 20}, *original)->node;
    assert(target != scene.RootId() && original->Find(target)->bounds.width == 50);
    const auto stats = scene.GetRenderStats();
    const auto pixels = scene.PixelsRevision();

    assert(scene.SetProperty(target, DslProperty::Action, std::string("after")));
    assert(scene.PendingDirty() == Dirty::Composite);
    const auto captured = scene.CaptureInputSnapshot();
    assert(captured != original && captured == scene.InputGeometry());
    assert(captured->scene == original->scene && captured->version > original->version);
    assert(captured->Find(target)->action == "after");
    assert(original->Find(target)->action == "before");
    assert(captured->Find(target)->bounds == original->Find(target)->bounds);
    assert(scene.GetRenderStats() == stats);
    assert(scene.PixelsRevision() == pixels);
    assert(scene.PendingDirty() == Dirty::Composite); // Freezing does not acknowledge submission.
    assert(scene.CaptureInputSnapshot() == captured);
    assert(scene.GetRenderStats() == stats);
    assert(scene.PixelsRevision() == pixels);

    assert(scene.SetProperty(target, DslProperty::Width, 100.0));
    ExpectCaptureNeedsLayout(scene);
    assert(scene.InputGeometry() == captured && captured->Find(target)->bounds.width == 50);
    const auto laid_out = Build(scene);
    assert(laid_out->Find(target)->bounds.width == 100);
    const auto resolved_stats = scene.GetRenderStats();
    const auto resolved_pixels = scene.PixelsRevision();
    assert(scene.CaptureInputSnapshot() == laid_out);
    assert(scene.GetRenderStats() == resolved_stats);
    assert(scene.PixelsRevision() == resolved_pixels);
}

void CheckCapturePaintGeometry()
{
    Scene scene(ParseBlueprint("IconButton(\"play\",\"go\",width:80,height:40)"), Shape);
    Layout(scene, {80, 40});
    const auto original = scene.InputGeometry();
    const auto target = scene.RootId();
    const auto stats = scene.GetRenderStats();
    assert(scene.HitTest({1, 1}, *original));

    assert(scene.SetProperty(target, DslProperty::Radius, 20.0));
    const auto radius_pixels = scene.PixelsRevision();
    const auto rounded = scene.CaptureInputSnapshot();
    assert(rounded->version > original->version);
    assert(rounded->Find(target)->radius == 20);
    assert(original->Find(target)->radius == 0);
    assert(!scene.HitTest({1, 1}, *rounded));
    assert(scene.HitTest({1, 1}, *original));
    assert(scene.GetRenderStats() == stats);
    assert(scene.PixelsRevision() == radius_pixels);

    assert(scene.SetProperty(target, DslProperty::Clip, true));
    const auto clip_pixels = scene.PixelsRevision();
    const auto clipped = scene.CaptureInputSnapshot();
    assert(clipped->version > rounded->version);
    assert(clipped->Find(target)->clip && clipped->Find(target)->radius == 20);
    assert(!rounded->Find(target)->clip);
    assert(scene.GetRenderStats() == stats);
    assert(scene.PixelsRevision() == clip_pixels);
    assert(Has(scene.PendingDirty(), Dirty::Paint));
    assert(scene.CaptureInputSnapshot() == clipped);
    assert(scene.GetRenderStats() == stats);
    assert(scene.PixelsRevision() == clip_pixels);
}

void CheckSubmittedGeometry()
{
    Scene scene(Buttons(), Shape);
    assert(!scene.InputGeometry());
    Layout(scene);
    const auto old = scene.InputGeometry();
    const auto first = scene.HitTest({20, 20}, *old)->node;
    const auto second = scene.HitTest({90, 20}, *old)->node;
    assert(old->version == 1 && old->scene);
    assert(old->Find(first)->parent == scene.RootId());

    Down(scene, old, {90, 20});
    assert(scene.State(second).captured && scene.State(second).hovered);
    assert(scene.SetProperty(first, DslProperty::Width, 100.0));
    const auto candidate = Build(scene);
    assert(candidate != old && candidate->version == old->version + 1);
    assert(scene.HitTest({90, 20}, *candidate)->node == first);
    assert(scene.HitTest({90, 20}, *old)->node == second);
    assert(scene.HitTest({90, 20}, *old)->localPosition == (contracts::LogicalPoint{20, 20}));
    assert(old->Find(first)->bounds.width == 50);
    assert(scene.State(second).hovered && !scene.State(first).hovered);

    // A rejected candidate cannot change the geometry of an already queued Up.
    ExpectActivation(Up(scene, old, {90, 20}), second, "two");
    Down(scene, old, {90, 20});
    assert(scene.ApplyInputSnapshot(candidate));
    assert(scene.State(first).hovered && !scene.State(second).hovered);
    assert(scene.State(second).captured && !scene.State(second).pressed);
    assert(!scene.ApplyInputSnapshot(old));
    assert(scene.State(first).hovered && !scene.State(second).hovered);
    assert(!Up(scene, candidate, {90, 20}).activation);
    assert(!scene.State(first).captured && !scene.State(second).captured);

    Down(scene, candidate, {90, 20});
    ExpectActivation(Up(scene, candidate, {90, 20}), first, "one");
    Down(scene, {}, {90, 20});
    assert(!Up(scene, {}, {90, 20}).activation);
    assert(!scene.HitTest({std::numeric_limits<double>::quiet_NaN(), 20}, *old));
}

void CheckSnapshotReuse()
{
    FakeClock clock;
    Scene scene(ParseBlueprint(R"(
        InteractionTarget(width:80,height:40,action:"go") {
            Visual(width:20,height:4,translateX:$offset,background:#FFFFFFFF)
                .transition(property:"translateX",durationMs:200,easing:"linear")
        }
    )"),
                Shape);
    Layout(scene);
    const auto initial = scene.InputGeometry();
    assert(initial->Find(scene.RootId())->children.empty());
    assert(!initial->Find({1, 1}));
    assert(Build(scene) == initial);

    scene.EnableAnimations(&clock);
    assert(scene.SetBinding("offset", 50.0));
    clock.now += 100'000'000;
    assert(scene.AdvanceAnimations(clock.now));
    assert(Build(scene) == initial);
    clock.now += 100'000'000;
    assert(scene.AdvanceAnimations(clock.now));
    assert(Build(scene) == initial);

    // A redundant layout still reuses the immutable geometry value.
    assert(scene.SetViewport({180, 61}));
    assert(scene.SetViewport({180, 60}));
    assert(Build(scene) == initial);
}

void CheckClipAndAvailability()
{
    Scene scene(ParseBlueprint(R"(
        Card(clip:true) { IconButton("play","old",width:80,height:40) }
    )"),
                Shape);
    Layout(scene, {80, 40});
    const auto old = scene.InputGeometry();
    const auto target = scene.HitTest({1, 1}, *old)->node;
    assert(scene.SetProperty(scene.RootId(), DslProperty::Radius, 20.0));
    const auto rounded = Build(scene);
    assert(scene.HitTest({1, 1}, *old));
    assert(!scene.HitTest({1, 1}, *rounded));
    Down(scene, old, {1, 1});
    ExpectActivation(Up(scene, old, {1, 1}), target, "old");

    Down(scene, rounded, {20, 20});
    assert(scene.SetProperty(target, DslProperty::Action, std::string("new")));
    assert(!Up(scene, rounded, {20, 20}).activation);
    Down(scene, rounded, {20, 20});
    assert(!Up(scene, rounded, {20, 20}).activation);
    const auto changed = Build(scene);
    assert(changed->version > rounded->version && changed->Find(target)->action == "new");
    Down(scene, changed, {20, 20});
    ExpectActivation(Up(scene, changed, {20, 20}), target, "new");

    Down(scene, changed, {20, 20});
    assert(scene.SetEnabled(scene.RootId(), false));
    assert(!scene.State(target).captured);
    assert(!Up(scene, changed, {20, 20}).activation);
    Down(scene, changed, {20, 20});
    assert(!Up(scene, changed, {20, 20}).activation);
    const auto disabled = Build(scene);
    assert(!disabled->Find(target)->enabled);
    assert(scene.SetEnabled(scene.RootId(), true));
    Down(scene, disabled, {20, 20});
    assert(!Up(scene, disabled, {20, 20}).activation);

    const auto enabled = Build(scene);
    Down(scene, enabled, {20, 20});
    assert(scene.SetProperty(scene.RootId(), DslProperty::Visible, false));
    assert(!Up(scene, enabled, {20, 20}).activation);
}

void CheckSceneAndNodeLifetime()
{
    Scene before(Buttons(), Shape);
    Scene after(Buttons(), Shape);
    Layout(before);
    Layout(after);
    assert(before.RootId() == after.RootId());
    assert(before.InputGeometry()->scene != after.InputGeometry()->scene);
    assert(!after.HitTest({20, 20}, *before.InputGeometry()));
    assert(!after.ApplyInputSnapshot(before.InputGeometry()));
    Down(after, before.InputGeometry(), {20, 20});
    assert(!Up(after, before.InputGeometry(), {20, 20}).activation);

    Blueprint region = ParseBlueprint("Card { IconButton(\"play\",\"old\",width:80,height:40) }");
    region.region = "body";
    Scene scene(region, Shape);
    Layout(scene);
    const auto old = scene.InputGeometry();
    const auto removed = scene.HitTest({20, 20}, *old)->node;
    Down(scene, old, {20, 20});
    const RegionUpdate update{"body",
                              ParseBlueprint("IconButton(\"play\",\"new\",width:80,height:40)")};
    assert(scene.MountRegions(std::span(&update, 1), {}));
    const auto next = Build(scene);
    const auto mounted = scene.HitTest({20, 20}, *next)->node;
    assert(mounted != removed && !scene.State(removed).enabled);
    assert(!Up(scene, old, {20, 20}).activation);
    Down(scene, old, {20, 20});
    assert(!Up(scene, old, {20, 20}).activation);
    Down(scene, next, {20, 20});
    ExpectActivation(Up(scene, next, {20, 20}), mounted, "new");
}

void CheckTouchCapture()
{
    Scene scene(Buttons(), Shape);
    Layout(scene);
    const auto snapshot = scene.InputGeometry();
    const auto first = scene.HitTest({20, 20}, *snapshot)->node;
    const auto second = scene.HitTest({90, 20}, *snapshot)->node;
    assert(!TouchUp(scene, snapshot).activation);
    TouchDown(scene, snapshot, {20, 20}, 10);
    TouchDown(scene, snapshot, {90, 20}, 11);
    assert(scene.State(first).captured && scene.State(first).pressed);
    assert(scene.State(second).captured && scene.State(second).pressed);
    assert(!scene.State(first).hovered && !scene.State(second).hovered);
    TouchDown(scene, snapshot, {90, 20}, 10); // Duplicate contact cannot retarget.
    assert(!TouchUp(scene, snapshot, 10, otherTouch).activation);
    assert(!TouchUp(scene, snapshot, 10, recreatedTouch).activation);
    ExpectActivation(TouchUp(scene, snapshot, 10), first, "one");
    assert(scene.State(second).captured);
    ExpectActivation(TouchUp(scene, snapshot, 11), second, "two");

    TouchDown(scene, snapshot, {20, 20});
    scene.HandleInput(contracts::TouchMotionEvent{window, {90, 20}, 0, 1, touch}, snapshot);
    assert(scene.State(first).captured && !scene.State(first).pressed);
    assert(!scene.State(second).captured);
    assert(!TouchUp(scene, snapshot).activation);
    TouchDown(scene, snapshot, {20, 20});
    scene.HandleInput(contracts::TouchMotionEvent{window, {90, 20}, 0, 1, touch}, snapshot);
    scene.HandleInput(contracts::TouchMotionEvent{window, {20, 20}, 0, 1, touch}, snapshot);
    ExpectActivation(TouchUp(scene, snapshot), first, "one");

    TouchDown(scene, snapshot, {20, 20});
    TouchDown(scene, snapshot, {90, 20}, 1);
    TouchDown(scene, snapshot, {20, 20}, 0, recreatedTouch);
    TouchDown(scene, snapshot, {90, 20}, 0, otherTouch);
    scene.HandleInput(contracts::TouchCancelEvent{window, 1, touch}, snapshot);
    assert(!TouchUp(scene, snapshot).activation);
    assert(!TouchUp(scene, snapshot, 1).activation);
    ExpectActivation(TouchUp(scene, snapshot, 0, recreatedTouch), first, "one");
    ExpectActivation(TouchUp(scene, snapshot, 0, otherTouch), second, "two");

    TouchDown(scene, snapshot, {20, 20});
    TouchDown(scene, snapshot, {90, 20}, 0, otherSeat);
    scene.HandleInput(contracts::FocusEvent{window, false, keyboard}, snapshot);
    assert(!TouchUp(scene, snapshot).activation);
    ExpectActivation(TouchUp(scene, snapshot, 0, otherSeat), second, "two");
}

void CheckTouchSnapshotAndKeyboard()
{
    Scene scene(Buttons(), Shape);
    Layout(scene);
    const auto old = scene.InputGeometry();
    const auto first = scene.HitTest({20, 20}, *old)->node;
    const auto second = scene.HitTest({90, 20}, *old)->node;
    TouchDown(scene, old, {90, 20});
    assert(scene.SetProperty(first, DslProperty::Width, 100.0));
    const auto next = Build(scene);
    assert(scene.State(second).pressed);
    ExpectActivation(TouchUp(scene, old), second, "two");
    TouchDown(scene, old, {90, 20});
    assert(scene.ApplyInputSnapshot(next));
    assert(scene.State(second).captured && !scene.State(second).pressed);
    assert(!TouchUp(scene, next).activation);
    TouchDown(scene, next, {90, 20});
    assert(scene.SetEnabled(first, false));
    assert(!TouchUp(scene, next).activation);

    assert(scene.SetEnabled(first, true));
    Build(scene);
    scene.CancelInput();
    scene.HandleInput(
        contracts::KeyEvent{window, 0x2b, contracts::ButtonState::Pressed, false, 1, keyboard},
        old);
    assert(scene.State(first).focused);
    scene.HandleInput(
        contracts::KeyEvent{window, 0x28, contracts::ButtonState::Pressed, false, 1, keyboard},
        old);
    assert(scene.SetProperty(first, DslProperty::Action, std::string("changed")));
    assert(!scene
                .HandleInput(contracts::KeyEvent{window, 0x28, contracts::ButtonState::Released,
                                                 false, 1, keyboard},
                             old)
                .activation);
    scene.CancelInput();
    scene.HandleInput(
        contracts::KeyEvent{window, 0x2b, contracts::ButtonState::Pressed, false, 1, keyboard}, {});
    assert(!scene.State(first).focused && !scene.State(second).focused);
}
} // namespace

int main()
{
    CheckCaptureWithoutBuild();
    CheckCapturePaintGeometry();
    CheckSubmittedGeometry();
    CheckSnapshotReuse();
    CheckClipAndAvailability();
    CheckSceneAndNodeLifetime();
    CheckTouchCapture();
    CheckTouchSnapshotAndKeyboard();
}
