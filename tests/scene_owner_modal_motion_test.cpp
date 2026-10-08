#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/owner_modal.hpp"
#include "prism/runtime/scene.hpp"

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

using namespace prism;
using namespace prism::runtime;

namespace {
constexpr contracts::WindowId window{1};
constexpr contracts::InputSource pointer{7, 1, 1};
constexpr contracts::InputSource keyboard{7, 2, 1};
constexpr contracts::InputSource other_pointer{9, 1, 1};
constexpr contracts::InputSource other_keyboard{9, 2, 1};
constexpr std::uint32_t enter_key = 0x28;
constexpr std::uint32_t escape_key = 0x29;
constexpr std::uint32_t tab_key = 0x2b;

ShapedText Shape(std::string_view text, double font)
{
    return {{}, text.size() * 7.0, font};
}

Blueprint Layout()
{
    auto root = ParseBlueprint(R"(
HStack(spacing:20) {
    VStack(width:200, spacing:0) {
        Button("Owner", action:"owner", height:40)
        TextField($owner_text, action:"owner-edit", height:40)
    }
    VStack(width:280, spacing:0) {
        Button("Choose", action:"choose", height:40)
        Button("Other choice", action:"other-choice", height:40)
        TextField($name, action:"name", height:40)
        Slider(action:"value", value:$value, height:44) {
            Visual(sliderPart:"track", height:4)
            Visual(sliderPart:"fill", height:4)
            Visual(sliderPart:"thumb", width:16, height:16)
        }
        InteractionTarget(action:"drag", height:40) {
            Visual(background:#456789FF)
        }.gesture(action:"drag-motion", threshold:6)
        ScrollView(height:80, scrollSpeed:1) {
            Text("Scrollable content", height:240)
        }
    }
}
)");
    Blueprint region;
    region.region = "task";
    region.properties.push_back({DslProperty::Width, 280.0});
    region.children.push_back(std::move(root.children.at(1)));
    root.children.at(1) = std::move(region);
    return root;
}

struct Fixture {
    Scene scene{Layout(), Shape};
    std::shared_ptr<const InputSnapshot> shown;
    contracts::NodeId scope;

    Fixture()
    {
        scene.SetBinding("owner_text", std::string("owner"));
        scene.SetBinding("name", std::string("draft"));
        scene.SetBinding("value", .2);
        assert(scene.SetViewport({520, 360}));
        Present();
        scope = scene.RegionId("task");
        assert(scope);
    }

    void Present()
    {
        scene.Build(window);
        scene.AcknowledgeComposite();
        shown = scene.CaptureInputSnapshot();
        assert(shown);
        scene.ApplyInputSnapshot(shown);
        assert(scene.IsInputSnapshotAdopted(*shown));
    }

    contracts::NodeId Find(std::string_view action) const
    {
        for (const auto &node : shown->nodes) {
            if (node.action == action) {
                return node.id;
            }
        }
        assert(false);
        return {};
    }

    contracts::LogicalPoint Point(contracts::NodeId node) const
    {
        const auto bounds = scene.Bounds(node);
        assert(bounds.width > 0 && bounds.height > 0);
        return {bounds.x + bounds.width / 2, bounds.y + bounds.height / 2};
    }

    InteractionResult Button(contracts::NodeId node, bool down,
                             contracts::InputSource source = pointer)
    {
        return scene.HandleInput(
            contracts::PointerButtonEvent{window, Point(node), contracts::PointerButton::Primary,
                                          down ? contracts::ButtonState::Pressed
                                               : contracts::ButtonState::Released,
                                          0, 1, source, 731},
            shown);
    }

    InteractionResult Click(contracts::NodeId node, contracts::InputSource source = pointer)
    {
        Button(node, true, source);
        return Button(node, false, source);
    }

    InteractionResult Key(std::uint32_t code, bool down = true, bool repeat = false,
                          contracts::InputSource source = keyboard)
    {
        return scene.HandleInput(contracts::KeyEvent{window,
                                                     code,
                                                     down ? contracts::ButtonState::Pressed
                                                          : contracts::ButtonState::Released,
                                                     repeat,
                                                     2,
                                                     source,
                                                     {}},
                                 shown);
    }

    std::uint64_t Begin()
    {
        const auto token = scene.BeginOwnerModal(scope, keyboard.seat);
        assert(token);
        Present();
        return *token;
    }

    void SliderDown(contracts::InputSource source = other_pointer)
    {
        const auto track = shown->Find(Find("value"))->slider_track;
        assert(track.width > 0);
        scene.HandleInput(contracts::PointerButtonEvent{window,
                                                        {track.x + track.width * .8, track.y},
                                                        contracts::PointerButton::Primary,
                                                        contracts::ButtonState::Pressed,
                                                        0,
                                                        3,
                                                        source},
                          shown);
    }

    contracts::NodeId Scroll() const
    {
        for (const auto &node : shown->nodes) {
            if (scene.ScrollInfo(node.id)) {
                return node.id;
            }
        }
        assert(false);
        return {};
    }
};

void ExpectNoInteraction(const InteractionResult &result)
{
    assert(!result.activation && !result.control_edit && !result.text_edit);
}

void ExpectClosure(Scene &scene, std::uint64_t token, OwnerModalCloseReason reason)
{
    const auto events = scene.TakeOwnerModalClosures();
    assert(events.size() == 1 && events.front().token == token && events.front().reason == reason);
    assert(scene.TakeOwnerModalClosures().empty());
}

void CheckDefaultAndExactToken()
{
    Fixture f;
    assert(!f.scene.SetOwnerModalInputReady(0, false));
    assert(!f.scene.SetOwnerModalInputReady(1, false));
    const auto token = f.Begin();
    const auto choose = f.Find("choose");
    assert(f.Click(choose).activation);
    assert(!f.scene.SetOwnerModalInputReady(token + 1, false));
    assert(f.Click(choose).activation);

    assert(f.scene.SetOwnerModalInputReady(token, false));
    assert(f.scene.SetOwnerModalInputReady(token, false));
    ExpectNoInteraction(f.Click(choose));
    assert(f.scene.State(choose).enabled && f.scene.OwnerModalToken() == token);
    assert(f.scene.TakeOwnerModalClosures().empty());
    assert(f.scene.SetOwnerModalInputReady(token, true));
    assert(f.scene.SetOwnerModalInputReady(token, true));
    assert(f.Click(choose).activation);
    assert(f.scene.EndOwnerModal(token));
    assert(!f.scene.SetOwnerModalInputReady(token, true));
}

void CheckBlockedInputAndAdoption()
{
    Fixture f;
    const auto token = f.Begin();
    const auto choose = f.Find("choose");
    const auto owner = f.Find("owner");
    const auto name = f.Find("name");
    const auto drag = f.Find("drag");
    const auto slider = f.Find("value");
    const auto scroll = f.Scroll();
    f.Click(name);
    assert(f.scene.State(name).focused);
    assert(f.scene.SetOwnerModalInputReady(token, false));

    // The enabled input descriptor remains preparable and adoptable while input is blocked.
    f.Present();
    assert(f.shown->Find(name)->enabled && f.shown->Find(name)->interactive);
    assert(f.scene.IsInputSnapshotAdopted(*f.shown));
    const auto version = f.shown->version;
    ExpectNoInteraction(f.scene.HandleInput(
        contracts::PointerEnterEvent{window, f.Point(choose), 4, pointer}, f.shown));
    ExpectNoInteraction(f.scene.HandleInput(
        contracts::PointerMotionEvent{window, f.Point(choose), 5, pointer}, f.shown));
    ExpectNoInteraction(f.Click(choose));
    ExpectNoInteraction(f.Click(owner));
    ExpectNoInteraction(f.Click(drag));
    ExpectNoInteraction(f.scene.HandleInput(
        contracts::PointerButtonEvent{window, f.Point(choose), contracts::PointerButton::Primary,
                                      contracts::ButtonState::Pressed, 0, 5, pointer}));
    ExpectNoInteraction(f.scene.HandleInput(
        contracts::PointerButtonEvent{window, f.Point(choose), contracts::PointerButton::Primary,
                                      contracts::ButtonState::Released, 0, 5, pointer}));
    assert(!f.scene.State(choose).hovered && !f.scene.State(owner).hovered);
    assert(!f.scene.State(drag).captured && !f.scene.State(drag).dragging);
    ExpectNoInteraction(f.Key(tab_key));
    ExpectNoInteraction(f.Key(tab_key, false));
    ExpectNoInteraction(f.Key(enter_key));
    ExpectNoInteraction(f.Key(enter_key, false));
    assert(f.scene.State(name).focused && !f.scene.State(name).pressed);
    ExpectNoInteraction(f.scene.HandleInput(
        contracts::TextInputEvent{window, "must not modify the draft", 6, keyboard}, f.shown));
    f.SliderDown();
    ExpectNoInteraction(f.scene.HandleInput(
        contracts::PointerMotionEvent{window, f.Point(slider), 7, other_pointer}, f.shown));
    ExpectNoInteraction(f.Button(slider, false, other_pointer));
    const auto offset = f.scene.ScrollInfo(scroll)->offset;
    ExpectNoInteraction(f.scene.HandleInput(
        contracts::PointerScrollEvent{window, f.Point(scroll), 0, 24, 8, pointer}, f.shown));
    assert(f.scene.ScrollInfo(scroll)->offset == offset);
    assert(f.scene.TakeControlEvents().empty() && f.scene.TakeGestureEvents().empty());

    // Unlocking after endpoint adoption does not invalidate that same geometry proof.
    assert(f.scene.SetOwnerModalInputReady(token, true));
    assert(f.scene.IsInputSnapshotAdopted(*f.shown));
    assert(f.scene.CaptureInputSnapshot()->version == version);
    const auto typed =
        f.scene.HandleInput(contracts::TextInputEvent{window, "X", 9, keyboard}, f.shown);
    assert(typed.text_edit && typed.text_edit->text.find('X') != std::string::npos);
    assert(typed.text_edit->text.find("must not") == std::string::npos);
    assert(f.Click(choose).activation);
}

void CheckUnlockDoesNotReplayPresses()
{
    Fixture f;
    const auto token = f.Begin();
    const auto choose = f.Find("choose");
    assert(f.scene.SetOwnerModalInputReady(token, false));
    ExpectNoInteraction(f.Button(choose, true));
    ExpectNoInteraction(f.Key(enter_key));
    assert(!f.scene.State(choose).pressed && !f.scene.State(choose).captured);

    assert(f.scene.SetOwnerModalInputReady(token, true));
    ExpectNoInteraction(f.Button(choose, false));
    ExpectNoInteraction(f.Key(enter_key, false));
    assert(f.scene.TakeGestureEvents().empty() && f.scene.TakeControlEvents().empty());
    assert(f.Click(choose).activation);
    f.Key(enter_key);
    assert(f.Key(enter_key, false).activation);
}

void CheckRefreshKeepsBlockedState()
{
    Fixture f;
    const auto token = f.Begin();
    assert(f.scene.SetOwnerModalInputReady(token, false));
    const auto old_input = f.shown;
    const auto refreshed = f.scene.RefreshOwnerModal(token);
    assert(refreshed && *refreshed != token);
    assert(!f.scene.SetOwnerModalInputReady(token, true));
    assert(!f.scene.IsInputSnapshotAdopted(*old_input));
    f.Present();
    const auto choose = f.Find("choose");
    ExpectNoInteraction(f.Click(choose));
    assert(f.scene.OwnerModalToken() == *refreshed);
    assert(f.scene.TakeOwnerModalClosures().empty());
    assert(f.scene.SetOwnerModalInputReady(*refreshed, true));
    assert(f.Click(choose).activation);
}

void CheckEscAllSeatsAndOpeningCancellation()
{
    Fixture f;
    const auto token = f.scene.BeginOwnerModal(f.scope, keyboard.seat);
    assert(token && f.scene.SetOwnerModalInputReady(*token, false));
    // The initiating seat supplies focus. Another owner-routed seat can cancel
    // during Opening, before the new input geometry has ever been adopted.
    assert(f.scene.State(f.Find("choose")).focused);
    ExpectNoInteraction(f.Key(escape_key, true, true, other_keyboard));
    assert(f.scene.OwnerModalToken() == *token);
    ExpectNoInteraction(f.scene.HandleInput(
        contracts::KeyEvent{
            window, escape_key, contracts::ButtonState::Pressed, false, 1, other_keyboard, {}},
        std::shared_ptr<const InputSnapshot>{}));
    assert(!f.scene.OwnerModalToken());
    ExpectClosure(f.scene, *token, OwnerModalCloseReason::Escape);

    f.Present();
    const auto successor = f.Begin();
    assert(f.scene.SetOwnerModalInputReady(successor, false));
    ExpectNoInteraction(f.Key(escape_key, true, true, other_keyboard));
    // Releasing a different source cannot retire the old physical Escape.
    ExpectNoInteraction(f.Key(escape_key, false));
    ExpectNoInteraction(f.Key(escape_key, true, false, other_keyboard));
    assert(f.scene.OwnerModalToken() == successor && f.scene.TakeOwnerModalClosures().empty());
    ExpectNoInteraction(f.Key(escape_key, false, false, other_keyboard));
    f.Key(escape_key, true, false, other_keyboard);
    ExpectClosure(f.scene, successor, OwnerModalCloseReason::Escape);
}

void CheckEscSourceGenerationAndOwnerRestoration()
{
    Fixture f;
    const auto owner = f.Find("owner");
    assert(f.Click(owner).activation);
    const auto token = f.Begin();
    f.Key(escape_key);
    ExpectClosure(f.scene, token, OwnerModalCloseReason::Escape);
    f.Present();
    assert(f.scene.State(owner).focused);

    const auto successor = f.Begin();
    constexpr contracts::InputSource new_keyboard{keyboard.seat, keyboard.device,
                                                  keyboard.generation + 1};
    ExpectNoInteraction(f.Key(escape_key, false, false, new_keyboard));
    ExpectNoInteraction(f.Key(escape_key));
    assert(f.scene.OwnerModalToken() == successor && f.scene.TakeOwnerModalClosures().empty());
    // A recreated keyboard is independent even when its seat/device numbers match.
    f.Key(escape_key, true, false, new_keyboard);
    ExpectClosure(f.scene, successor, OwnerModalCloseReason::Escape);
    f.Present();
    ExpectNoInteraction(f.Key(escape_key, false));
    ExpectNoInteraction(f.Key(escape_key, false, false, new_keyboard));
    assert(f.scene.State(owner).focused && f.scene.TakeOwnerModalClosures().empty());
    assert(f.Click(owner).activation);
}

void CheckDefaultSeatAndNativeKeyboard()
{
    Fixture f;
    const auto token = f.scene.BeginOwnerModal(f.scope);
    assert(token);
    f.Present();
    assert(f.scene.SetOwnerModalInputReady(*token, false));
    // Shared Host providers use the default seat 0; the Wayland adapter assigns
    // nonzero seat identities. Both remain in this same owner-local modal domain.
    constexpr contracts::InputSource native_keyboard{1, 2, 1};
    ExpectNoInteraction(f.Key(escape_key, true, false, native_keyboard));
    ExpectClosure(f.scene, *token, OwnerModalCloseReason::Escape);
    f.Present();
    ExpectNoInteraction(f.Key(escape_key, false, false, native_keyboard));
    assert(f.Click(f.Find("owner")).activation);
}

void CheckBlockingCancelsExistingStreams()
{
    Fixture f;
    const auto token = f.Begin();
    const auto drag = f.Find("drag");
    const auto slider = f.Find("value");
    f.Button(drag, true);
    const auto start = f.Point(drag);
    f.scene.HandleInput(contracts::PointerMotionEvent{window, {start.x + 12, start.y}, 1, pointer},
                        f.shown);
    const auto begun = f.scene.TakeGestureEvents();
    assert(begun.size() == 1 && begun.front().phase == contracts::GesturePhase::Begin);
    f.SliderDown();
    const auto preview = f.scene.TakeControlEvents();
    assert(preview.size() == 1 && preview.front().event.phase == ValuePhase::Preview);
    assert(f.scene.State(drag).dragging && f.scene.State(slider).captured);

    assert(f.scene.SetOwnerModalInputReady(token, false));
    const auto cancelled_gesture = f.scene.TakeGestureEvents();
    assert(cancelled_gesture.size() == 1);
    assert(cancelled_gesture.front().id == begun.front().id);
    assert(cancelled_gesture.front().phase == contracts::GesturePhase::Cancel);
    const auto cancelled_value = f.scene.TakeControlEvents();
    assert(cancelled_value.size() == 1);
    assert(cancelled_value.front().event.phase == ValuePhase::Cancel);
    assert(cancelled_value.front().event.interaction == preview.front().event.interaction);
    assert(cancelled_value.front().event.value == preview.front().event.before);
    assert(!f.scene.State(drag).captured && !f.scene.State(slider).captured);
    ExpectNoInteraction(f.Key(0x4f, true, false, other_keyboard));
    ExpectNoInteraction(f.Key(0x4f, false, false, other_keyboard));
    assert(f.scene.TakeControlEvents().empty());

    assert(f.scene.SetOwnerModalInputReady(token, true));
    ExpectNoInteraction(f.Button(drag, false));
    ExpectNoInteraction(f.Button(slider, false, other_pointer));
    assert(f.scene.TakeGestureEvents().empty() && f.scene.TakeControlEvents().empty());
}

void CheckLifecycleCleanupAndFocus()
{
    Fixture f;
    const auto token = f.Begin();
    const auto choose = f.Find("choose");
    const auto name = f.Find("name");
    f.Click(name);
    assert(f.scene.SetOwnerModalInputReady(token, false));
    ExpectNoInteraction(f.scene.HandleInput(contracts::FocusEvent{window, false, keyboard},
                                            std::shared_ptr<const InputSnapshot>{}));
    assert(!f.scene.State(name).focused);
    ExpectNoInteraction(f.scene.HandleInput(contracts::FocusEvent{window, true, keyboard},
                                            std::shared_ptr<const InputSnapshot>{}));
    assert(f.scene.State(choose).focused && !f.scene.State(f.Find("owner")).focused);
    ExpectNoInteraction(f.scene.HandleInput(contracts::PointerLeaveEvent{window, 1, pointer},
                                            std::shared_ptr<const InputSnapshot>{}));
    ExpectNoInteraction(f.scene.HandleInput(contracts::PointerCancelEvent{window, 2, pointer},
                                            std::shared_ptr<const InputSnapshot>{}));
    ExpectNoInteraction(f.scene.HandleInput(contracts::CloseRequestedEvent{window},
                                            std::shared_ptr<const InputSnapshot>{}));
    assert(!f.scene.State(choose).focused);
    assert(f.scene.OwnerModalToken() == token && f.scene.TakeOwnerModalClosures().empty());
    assert(f.scene.SetOwnerModalInputReady(token, true));
    assert(f.Click(choose).activation);
}

void CheckClosingCannotReplayIntoOwner()
{
    Fixture f;
    const auto token = f.Begin();
    const auto choose = f.Find("choose");
    assert(f.scene.SetOwnerModalInputReady(token, false));
    ExpectNoInteraction(f.Button(choose, true));
    ExpectNoInteraction(f.Key(enter_key));
    assert(f.scene.EndOwnerModal(token));
    ExpectClosure(f.scene, token, OwnerModalCloseReason::Ended);
    f.Present();
    const auto owner = f.Find("owner");
    ExpectNoInteraction(f.Button(owner, false));
    ExpectNoInteraction(f.Key(enter_key, false));
    assert(!f.scene.SetOwnerModalInputReady(token, true));
    assert(f.Click(owner).activation);
}
} // namespace

int main()
{
    CheckDefaultAndExactToken();
    CheckBlockedInputAndAdoption();
    CheckUnlockDoesNotReplayPresses();
    CheckRefreshKeepsBlockedState();
    CheckEscAllSeatsAndOpeningCancellation();
    CheckEscSourceGenerationAndOwnerRestoration();
    CheckDefaultSeatAndNativeKeyboard();
    CheckBlockingCancelsExistingStreams();
    CheckLifecycleCleanupAndFocus();
    CheckClosingCannotReplayIntoOwner();
}
