#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/owner_modal.hpp"
#include "prism/runtime/scene.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

using namespace prism;
using namespace prism::runtime;

namespace {
constexpr contracts::WindowId window{1};
constexpr contracts::InputSource pointer{0, 1, 1};
constexpr contracts::InputSource keyboard{0, 2, 1};
constexpr contracts::InputSource other_pointer{2, 1, 1};
constexpr contracts::InputSource other_keyboard{2, 2, 1};
constexpr std::uint32_t enter_key = 0x28;
constexpr std::uint32_t escape_key = 0x29;
constexpr std::uint32_t tab_key = 0x2b;

ShapedText Shape(std::string_view text, double font)
{
    if (text == "reject") {
        throw std::runtime_error("Fixture rejects candidate text");
    }
    return {{}, text.size() * 7.0, font};
}

contracts::ThemeSnapshot Theme(std::uint64_t generation = 1)
{
    contracts::ThemeSnapshot theme;
    theme.id = "owner-modal-fixture";
    theme.name = "Owner Modal Fixture";
    theme.generation = generation;
    theme.colors = {{"surface", {20, 30, 40, 255}}};
    return theme;
}

Blueprint Layout()
{
    auto root = ParseBlueprint(R"(
HStack(spacing:20, background:"@surface") {
    VStack(width:220, spacing:0) {
        Button("Outside first", action:"outside-first", height:40)
        Button("Outside second", action:"outside-second", height:40)
        TextField($outside_text, action:"outside-edit", height:40)
        Slider(action:"outside-value", value:$outside_value, height:44) {
            Visual(sliderPart:"track", height:4)
            Visual(sliderPart:"fill", height:4)
            Visual(sliderPart:"thumb", width:16, height:16)
        }
        InteractionTarget(action:"outside-drag", height:40) {
            Visual(background:#123456FF)
        }.gesture(action:"outside-gesture", threshold:6)
        Button("Outside popup", action:"outside-open", height:40)
        ScrollView(height:80, scrollSpeed:1) {
            Text("Outside scroll content", height:240)
        }
    }
    VStack(width:240, spacing:0) {
        VStack(spacing:0, visible:$scope_visible, enabled:$scope_enabled) {
            Button("Inside first", action:"inside-first", height:40)
            Button("Inside second", action:"inside-second", height:40)
            TextField($inside_text, action:"inside-edit", height:40)
            Slider(action:"inside-value", value:$inside_value, height:44) {
                Visual(sliderPart:"track", height:4)
                Visual(sliderPart:"fill", height:4)
                Visual(sliderPart:"thumb", width:16, height:16)
            }
            InteractionTarget(action:"inside-drag", height:40) {
                Visual(background:#456789FF)
            }.gesture(action:"inside-gesture", threshold:6)
            Button("Inside popup", action:"inside-open", height:40)
            ScrollView(height:80, scrollSpeed:1) {
                Text("Inside scroll content", height:240)
            }
        }
        Button("Stable return entry", action:"outside-return", height:40)
    }
    Popup("outside-open", width:200, height:100) {
        Button("Outside popup command", action:"outside-popup-command", height:40)
    }
    Popup("inside-open", width:200, height:100) {
        Button("Inside popup command", action:"inside-popup-command", height:40)
    }
}
)");
    Blueprint region;
    region.region = "scope-slot";
    region.properties.push_back({DslProperty::Width, 240.0});
    region.children.push_back(std::move(root.children.at(1)));
    root.children.at(1) = std::move(region);
    return root;
}

InteractionResult Button(Scene &scene, const std::shared_ptr<const InputSnapshot> &shown,
                         contracts::LogicalPoint point, bool down,
                         contracts::InputSource source = pointer)
{
    return scene.HandleInput(contracts::PointerButtonEvent{window, point,
                                                           contracts::PointerButton::Primary,
                                                           down ? contracts::ButtonState::Pressed
                                                                : contracts::ButtonState::Released,
                                                           0, 1, source, 731},
                             shown);
}

struct Fixture {
    Scene scene{Layout(), Shape, {}, Theme()};
    std::shared_ptr<const InputSnapshot> shown;
    contracts::NodeId scope;

    Fixture()
    {
        scene.SetBinding("scope_visible", true);
        scene.SetBinding("scope_enabled", true);
        scene.SetBinding("outside_text", std::string("outside"));
        scene.SetBinding("inside_text", std::string("inside"));
        scene.SetBinding("outside_value", .2);
        scene.SetBinding("inside_value", .2);
        assert(scene.SetViewport({520, 380}));
        Present();
        scope = shown->Find(Find("inside-first"))->parent;
        assert(scope && scope != scene.RootId());
    }

    void Present()
    {
        scene.Build(window);
        scene.AcknowledgeComposite();
        shown = scene.InputGeometry();
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

    contracts::LogicalPoint Point(contracts::NodeId id) const
    {
        const auto bounds = scene.Bounds(id);
        assert(bounds.width > 0 && bounds.height > 0);
        return {bounds.x + bounds.width / 2, bounds.y + bounds.height / 2};
    }

    InteractionResult Click(contracts::NodeId id, contracts::InputSource source = pointer)
    {
        Button(scene, shown, Point(id), true, source);
        return Button(scene, shown, Point(id), false, source);
    }

    InteractionResult Key(std::uint32_t code, bool down = true, bool repeat = false,
                          contracts::InputSource source = keyboard, bool shift = false)
    {
        return scene.HandleInput(contracts::KeyEvent{window,
                                                     code,
                                                     down ? contracts::ButtonState::Pressed
                                                          : contracts::ButtonState::Released,
                                                     repeat,
                                                     2,
                                                     source,
                                                     {shift}},
                                 shown);
    }

    std::uint64_t Begin()
    {
        const auto token = scene.BeginOwnerModal(scope);
        assert(token && *token && scene.OwnerModalToken() == *token);
        Present();
        assert(shown->owner_modal_epoch == scene.OwnerModalEpoch());
        return *token;
    }

    contracts::NodeId Scroll(bool inside) const
    {
        const auto parent = inside ? scope : shown->Find(Find("outside-first"))->parent;
        for (const auto &node : shown->nodes) {
            if (node.parent == parent && scene.ScrollInfo(node.id)) {
                return node.id;
            }
        }
        assert(false);
        return {};
    }

    void Wheel(contracts::NodeId id)
    {
        scene.HandleInput(contracts::PointerScrollEvent{window, Point(id), 0, 24, 3, pointer},
                          shown);
    }

    void SliderDown(contracts::NodeId id, contracts::InputSource source = pointer)
    {
        const auto track = shown->Find(id)->slider_track;
        assert(track.width > 0);
        Button(scene, shown, {track.x + track.width * .8, track.y}, true, source);
    }

    contracts::GestureEvent Drag(contracts::NodeId id, contracts::InputSource source = pointer)
    {
        const auto point = Point(id);
        Button(scene, shown, point, true, source);
        scene.HandleInput(contracts::PointerMotionEvent{window, {point.x + 10, point.y}, 4, source},
                          shown);
        const auto events = scene.TakeGestureEvents();
        assert(events.size() == 1 && events.front().phase == contracts::GesturePhase::Begin);
        assert(events.front().node == id && scene.State(id).dragging);
        return events.front();
    }
};

void ExpectClosure(Scene &scene, std::uint64_t token, OwnerModalCloseReason reason)
{
    const auto closures = scene.TakeOwnerModalClosures();
    assert(closures.size() == 1 && closures.front().token == token);
    assert(closures.front().reason == reason);
    assert(scene.TakeOwnerModalClosures().empty());
}

ControlEdit ExpectControl(Scene &scene, contracts::NodeId node, ValuePhase phase)
{
    const auto events = scene.TakeControlEvents();
    assert(events.size() == 1 && events.front().node == node);
    assert(events.front().event.phase == phase);
    return events.front();
}

void ExpectGestureCancel(Scene &scene, const contracts::GestureEvent &begin)
{
    const auto events = scene.TakeGestureEvents();
    assert(events.size() == 1 && events.front().phase == contracts::GesturePhase::Cancel);
    assert(events.front().id == begin.id && events.front().node == begin.node);
    assert(!scene.State(begin.node).dragging && !scene.State(begin.node).captured);
}

void CheckSnapshotAdoption()
{
    Fixture f;
    const auto before = f.shown;
    assert(f.scene.IsInputSnapshotAdopted(*before));
    const auto token = f.scene.BeginOwnerModal(f.scope);
    assert(token && !f.scene.IsInputSnapshotAdopted(*before));
    f.scene.Build(window);
    const auto modal = f.scene.InputGeometry();
    assert(modal && modal->owner_modal_epoch == f.scene.OwnerModalEpoch());
    // Adopting metadata without a stationary pointer does not change pixels.
    assert(!f.scene.ApplyInputSnapshot(modal));
    assert(f.scene.IsInputSnapshotAdopted(*modal));
    assert(!f.scene.ApplyInputSnapshot(modal));
    assert(f.scene.IsInputSnapshotAdopted(*modal));

    auto changed_epoch = std::make_shared<InputSnapshot>(*before);
    changed_epoch->owner_modal_epoch = f.scene.OwnerModalEpoch();
    assert(!f.scene.IsInputSnapshotAdopted(*changed_epoch));
    assert(!f.scene.ApplyInputSnapshot(changed_epoch));
    assert(f.scene.IsInputSnapshotAdopted(*modal));
    assert(f.scene.EndOwnerModal(*token));
    assert(!f.scene.IsInputSnapshotAdopted(*modal));
    f.Present();
    auto ended_epoch = std::make_shared<InputSnapshot>(*modal);
    ended_epoch->owner_modal_epoch = f.scene.OwnerModalEpoch();
    assert(!f.scene.IsInputSnapshotAdopted(*ended_epoch));
    assert(!f.scene.ApplyInputSnapshot(ended_epoch));
    assert(f.scene.IsInputSnapshotAdopted(*f.shown));
    ExpectClosure(f.scene, *token, OwnerModalCloseReason::Ended);
}

void CheckScopeAndSnapshots()
{
    Fixture f;
    Fixture foreign;
    assert(!f.scene.OwnerModalToken() && !f.scene.OwnerModalEpoch());
    assert(f.shown->owner_modal_epoch == 0);
    assert(!f.scene.BeginOwnerModal({}));
    assert(!f.scene.BeginOwnerModal({UINT32_MAX, 1}));
    assert(!f.scene.EndOwnerModal(0));
    assert(f.scene.SetProperty(f.scene.RootId(), DslProperty::Background,
                               contracts::Color{0, 0, 0, 0}));
    f.Present();

    const auto outside = f.Find("outside-first");
    const auto inside = f.Find("inside-first");
    assert(f.Click(outside).activation->action == "outside-first");
    const auto outside_bounds = f.scene.Bounds(outside);
    const auto before = f.shown;
    f.Key(enter_key); // A background key press cannot survive opening the scope.
    assert(f.scene.State(outside).pressed);
    const auto token = f.Begin();
    const auto epoch = f.scene.OwnerModalEpoch();
    assert(epoch > before->owner_modal_epoch && f.scene.State(inside).focused);
    assert(!f.scene.State(outside).pressed && !f.scene.State(outside).focused);
    assert(f.scene.IsVisible(outside) && f.scene.Bounds(outside) == outside_bounds);
    bool shield = false;
    for (const auto &region : f.scene.InputRegions()) {
        if (region.bounds == f.scene.Bounds(f.scene.RootId())) {
            assert(region.corner_radius == 0);
            shield = true;
        }
    }
    assert(shield); // Transparent space must also consume this window's outside input.
    assert(!f.scene.BeginOwnerModal(inside));
    assert(!f.scene.BeginOwnerModal(f.scope));
    assert(!f.scene.EndOwnerModal(token + 1));
    assert(f.scene.OwnerModalToken() == token && f.scene.OwnerModalEpoch() == epoch);
    assert(f.scene.TakeOwnerModalClosures().empty());

    // Both live and immutable hit testing use the same scope. Frozen input from
    // before the transition, null input, and a different Scene cannot enter it.
    assert(!f.scene.HitTest(f.Point(outside)));
    assert(!f.scene.HitTest(f.Point(outside), *f.shown));
    assert(f.scene.HitTest(f.Point(inside), *f.shown)->node == inside);
    for (const auto &rejected : {before, foreign.shown, std::shared_ptr<const InputSnapshot>{}}) {
        Button(f.scene, rejected, f.Point(inside), true);
        assert(!Button(f.scene, rejected, f.Point(inside), false).activation);
        assert(!f.scene.HandleInput(contracts::TextInputEvent{window, "blocked", 5}, rejected)
                    .text_edit);
    }
    assert(!f.scene.ApplyInputSnapshot(before));
    assert(!f.scene.ApplyInputSnapshot(foreign.shown));
    assert(!f.Key(enter_key, false).activation);
    assert(!f.Click(outside).activation);
    assert(f.Click(inside).activation->action == "inside-first");

    const auto outside_scroll = f.Scroll(false);
    const auto inside_scroll = f.Scroll(true);
    const auto offset = f.scene.ScrollInfo(outside_scroll)->offset;
    f.Wheel(outside_scroll);
    assert(f.scene.ScrollInfo(outside_scroll)->offset == offset);
    assert(f.scene.OwnerModalToken() == token && f.scene.TakeOwnerModalClosures().empty());
    f.Wheel(inside_scroll);
    assert(f.scene.ScrollInfo(inside_scroll)->offset > 0);
    f.Present();
    assert(f.scene.OwnerModalEpoch() == epoch);
    assert(!f.scene.OpenPopup(f.Find("inside-open")));
    assert(!f.scene.OpenPopup(f.Find("outside-open")));

    const auto modal_input = f.shown;
    assert(f.scene.EndOwnerModal(token));
    assert(!f.scene.OwnerModalToken() && f.scene.OwnerModalEpoch() > epoch);
    assert(!f.scene.EndOwnerModal(token));
    ExpectClosure(f.scene, token, OwnerModalCloseReason::Ended);
    f.Present();
    assert(f.scene.State(outside).focused);
    Button(f.scene, modal_input, f.Point(inside), true);
    assert(!Button(f.scene, modal_input, f.Point(inside), false).activation);
    assert(!f.scene.ApplyInputSnapshot(modal_input));
    assert(f.Click(outside).activation->action == "outside-first");
    const auto next = f.Begin();
    assert(next != token);
    assert(f.scene.EndOwnerModal(next));
    ExpectClosure(f.scene, next, OwnerModalCloseReason::Ended);
}

void CheckKeyboardAndText()
{
    Fixture f;
    const auto outside_editor = f.Find("outside-edit");
    f.Click(outside_editor);
    const auto token = f.Begin();
    assert(
        !f.scene.HandleInput(contracts::TextInputEvent{window, "ignored", 1}, f.shown).text_edit);
    assert(!f.scene.SetBinding("outside_text", std::string("outside")));

    f.Key(enter_key);
    assert(!f.Key(enter_key, true, true).activation);
    const auto activation = f.Key(enter_key, false);
    assert(activation.activation && activation.activation->action == "inside-first");
    std::set<std::string> visited;
    for (unsigned index = 0; index < 12; ++index) {
        f.Key(tab_key);
        f.Key(tab_key, false);
        const auto focused = f.scene.FocusedAction();
        assert(focused && focused->starts_with("inside-"));
        visited.insert(*focused);
    }
    assert(visited.size() == 6 && f.scene.FocusedAction() == "inside-first");
    f.Key(tab_key, true, false, keyboard, true);
    f.Key(tab_key, false, false, keyboard, true);
    assert(f.scene.FocusedAction() == "inside-open");

    const auto inside_editor = f.Find("inside-edit");
    f.Click(inside_editor);
    const auto typed = f.scene.HandleInput(contracts::TextInputEvent{window, "X", 2}, f.shown);
    assert(typed.text_edit && typed.text_edit->action == "inside-edit");
    assert(typed.text_edit->text.find('X') != std::string::npos);
    assert(!f.scene.SetBinding("outside_text", std::string("outside")));
    f.Present();
    const auto epoch = f.scene.OwnerModalEpoch();
    f.Key(escape_key);
    assert(!f.scene.OwnerModalToken() && f.scene.OwnerModalEpoch() > epoch);
    ExpectClosure(f.scene, token, OwnerModalCloseReason::Escape);
    f.Present();
    assert(f.scene.State(outside_editor).focused);
    const auto second = f.Begin(); // Reopen before the physical Escape is released.
    assert(!f.Key(escape_key, true, true).activation);
    assert(!f.Key(escape_key, false).activation);
    assert(f.scene.TakeOwnerModalClosures().empty());
    f.Key(escape_key, true, true); // A repeat cannot create a new close decision.
    assert(f.scene.OwnerModalToken() == second);
    assert(f.scene.EndOwnerModal(second));
    ExpectClosure(f.scene, second, OwnerModalCloseReason::Ended);
}

void CheckAllSeatsAndFocusLoss()
{
    Fixture f;
    const auto outside = f.Find("outside-first");
    const auto other_outside = f.Find("outside-second");
    f.Click(outside);
    Button(f.scene, f.shown, f.Point(other_outside), true, other_pointer);
    assert(f.scene.State(other_outside).captured);
    const auto token = f.Begin();
    const auto epoch = f.scene.OwnerModalEpoch();
    assert(!f.scene.State(other_outside).captured);
    assert(!Button(f.scene, f.shown, f.Point(other_outside), false, other_pointer).activation);
    assert(!f.Click(other_outside, other_pointer).activation);
    assert(!f.scene.State(outside).focused && !f.scene.State(other_outside).focused);

    f.scene.HandleInput(contracts::FocusEvent{window, true, other_keyboard}, f.shown);
    f.Key(enter_key, true, false, other_keyboard);
    const auto activated = f.Key(enter_key, false, false, other_keyboard);
    assert(activated.activation && activated.activation->action.starts_with("inside-"));
    const auto begin = f.Drag(f.Find("inside-drag"), other_pointer);
    f.scene.HandleInput(contracts::FocusEvent{window, false, keyboard}, f.shown);
    assert(f.scene.State(begin.node).dragging); // Losing another seat does not cancel this stream.
    f.scene.HandleInput(contracts::FocusEvent{window, false, other_keyboard}, f.shown);
    ExpectGestureCancel(f.scene, begin);
    assert(f.scene.OwnerModalToken() == token && f.scene.OwnerModalEpoch() == epoch);
    assert(f.scene.TakeOwnerModalClosures().empty());
    assert(!Button(f.scene, f.shown, f.Point(begin.node), false, other_pointer).activation);
    f.scene.HandleInput(contracts::FocusEvent{window, true, keyboard}, f.shown);
    assert(f.scene.FocusedAction() && f.scene.FocusedAction()->starts_with("inside-"));
    assert(!f.Click(outside, other_pointer).activation);
    assert(f.scene.EndOwnerModal(token));
    ExpectClosure(f.scene, token, OwnerModalCloseReason::Ended);
    assert(f.scene.State(outside).focused && f.scene.State(other_outside).focused);
}

void CheckCaptureCancellation()
{
    {
        Fixture f;
        const auto slider = f.Find("outside-value");
        f.SliderDown(slider);
        const auto preview = ExpectControl(f.scene, slider, ValuePhase::Preview);
        const auto token = f.Begin();
        const auto cancel = ExpectControl(f.scene, slider, ValuePhase::Cancel);
        assert(cancel.event.interaction == preview.event.interaction);
        assert(cancel.event.value == preview.event.before);
        assert(cancel.event.reason == ValueCancelReason::Unavailable);
        assert(!f.scene.State(slider).captured);
        Button(f.scene, f.shown, f.Point(slider), false);
        assert(f.scene.TakeControlEvents().empty());
        assert(f.scene.EndOwnerModal(token));
    }
    {
        Fixture f;
        for (unsigned stop = 0; stop < 4; ++stop) {
            assert(f.scene.FocusNext());
        }
        const auto slider = f.Find("outside-value");
        assert(f.scene.State(slider).focused);
        f.Key(0x4f); // A keyboard value proposal is cancelled at the same domain boundary.
        const auto preview = ExpectControl(f.scene, slider, ValuePhase::Preview);
        const auto token = f.Begin();
        const auto cancel = ExpectControl(f.scene, slider, ValuePhase::Cancel);
        assert(cancel.event.interaction == preview.event.interaction);
        f.Key(0x4f, false);
        assert(f.scene.TakeControlEvents().empty());
        assert(f.scene.EndOwnerModal(token));
    }
    {
        Fixture f;
        const auto begin = f.Drag(f.Find("outside-drag"));
        const auto token = f.Begin();
        ExpectGestureCancel(f.scene, begin);
        assert(!Button(f.scene, f.shown, f.Point(begin.node), false).activation);
        assert(f.scene.TakeGestureEvents().empty());
        assert(f.scene.EndOwnerModal(token));
    }
    for (const bool escape : {false, true}) {
        Fixture f;
        const auto token = f.Begin();
        const auto slider = f.Find("inside-value");
        f.SliderDown(slider);
        const auto preview = ExpectControl(f.scene, slider, ValuePhase::Preview);
        if (escape) {
            f.Key(escape_key);
        } else {
            assert(f.scene.EndOwnerModal(token));
        }
        const auto cancel = ExpectControl(f.scene, slider, ValuePhase::Cancel);
        assert(cancel.event.interaction == preview.event.interaction);
        assert(cancel.event.value == preview.event.before);
        ExpectClosure(f.scene, token,
                      escape ? OwnerModalCloseReason::Escape : OwnerModalCloseReason::Ended);
        f.Present();
        Button(f.scene, f.shown, f.Point(slider), false);
        assert(f.scene.TakeControlEvents().empty());
        assert(!f.scene.State(slider).captured && !f.scene.State(slider).dragging);
    }
    {
        Fixture f;
        const auto token = f.Begin();
        const auto first = f.Find("inside-first");
        f.Key(enter_key);
        assert(f.scene.State(first).pressed);
        assert(f.scene.EndOwnerModal(token));
        f.Present();
        assert(!f.Key(enter_key, false).activation);
        assert(!f.scene.State(first).pressed);
        ExpectClosure(f.scene, token, OwnerModalCloseReason::Ended);
    }
    {
        Fixture f;
        const auto token = f.Begin();
        const auto begin = f.Drag(f.Find("inside-drag"));
        assert(f.scene.EndOwnerModal(token));
        ExpectGestureCancel(f.scene, begin);
        f.Present();
        assert(!Button(f.scene, f.shown, f.Point(begin.node), false).activation);
        assert(f.scene.TakeGestureEvents().empty());
        ExpectClosure(f.scene, token, OwnerModalCloseReason::Ended);
    }
}

void CheckAvailabilityAndRestoration()
{
    for (unsigned condition = 0; condition < 3; ++condition) {
        Fixture f;
        const auto token = f.Begin();
        const auto epoch = f.scene.OwnerModalEpoch();
        if (condition == 0) {
            assert(f.scene.SetBinding("scope_visible", false));
        } else if (condition == 1) {
            assert(f.scene.SetBinding("scope_enabled", false));
        } else {
            assert(f.scene.SetEnabled(f.scene.RegionId("scope-slot"), false));
        }
        f.Present();
        assert(!f.scene.OwnerModalToken() && f.scene.OwnerModalEpoch() > epoch);
        ExpectClosure(f.scene, token, OwnerModalCloseReason::Unavailable);
        assert(!f.scene.EndOwnerModal(token));
        assert(!f.scene.BeginOwnerModal(f.scope));
        assert(f.scene.TakeOwnerModalClosures().empty());
    }

    Fixture f;
    const auto first = f.Find("outside-first");
    const auto fallback = f.Find("outside-return");
    f.Click(first);
    const auto token = f.Begin();
    assert(f.scene.SetEnabled(first, false));
    f.Present();
    assert(f.scene.OwnerModalToken() == token);
    assert(f.scene.EndOwnerModal(token));
    assert(!f.scene.State(first).focused && f.scene.State(fallback).focused);
    ExpectClosure(f.scene, token, OwnerModalCloseReason::Ended);
}

void CheckTransactionsAndPopupExclusion()
{
    Fixture f;
    assert(f.scene.OpenPopup(f.Find("outside-open")));
    f.Present();
    const auto popup_input = f.shown;
    const auto command = popup_input->Find(f.Find("outside-popup-command"));
    assert(command);
    const auto bounds = command->bounds;
    const contracts::LogicalPoint popup_point{bounds.x + bounds.width / 2,
                                              bounds.y + bounds.height / 2};
    assert(f.scene.PopupToken());
    const auto token = f.Begin();
    const auto epoch = f.scene.OwnerModalEpoch();
    assert(!f.scene.PopupToken());
    Button(f.scene, popup_input, popup_point, true);
    assert(!Button(f.scene, popup_input, popup_point, false).activation);

    assert(f.scene.Preflight({{"inside_value", .4}}));
    assert(f.scene.ApplyTheme(Theme(2)));
    assert(f.scene.SetViewport({540, 400}));
    f.Present();
    assert(f.scene.OwnerModalToken() == token && f.scene.OwnerModalEpoch() == epoch);
    assert(f.shown->owner_modal_epoch == epoch && f.scene.TakeOwnerModalClosures().empty());

    // Failed preparation must leave the live scope and its generation intact.
    std::string diagnostic;
    assert(!f.scene.Preflight({{"scope_visible", false}, {"inside_value", std::string("bad")}},
                              &diagnostic));
    assert(!diagnostic.empty());
    auto missing_token = Theme(3);
    missing_token.colors.clear();
    assert(!f.scene.ApplyTheme(missing_token, &diagnostic));
    const RegionUpdate bad{"scope-slot", ParseBlueprint(R"(
            VStack {
                Button("reject", action:"bad")
                Button("Retained popup anchor", action:"inside-open", height:40)
            }
        )")};
    assert(!f.scene.MountRegions(std::span(&bad, 1), {}, &diagnostic));
    assert(!diagnostic.empty());
    assert(f.scene.OwnerModalToken() == token && f.scene.OwnerModalEpoch() == epoch);
    assert(f.scene.IsVisible(f.scope) && !f.scene.RegionMounted("scope-slot"));
    assert(f.scene.TakeOwnerModalClosures().empty());

    const auto old = f.shown;
    const RegionUpdate update{"scope-slot", ParseBlueprint(R"(
            VStack {
                Button("Replacement", action:"replacement", height:40)
                Button("Retained popup anchor", action:"inside-open", height:40)
            }
        )")};
    assert(f.scene.MountRegions(std::span(&update, 1), {}, &diagnostic));
    f.Present();
    assert(!f.scene.IsVisible(f.scope) && !f.scene.OwnerModalToken());
    assert(f.scene.OwnerModalEpoch() > epoch && f.scene.RegionMounted("scope-slot"));
    ExpectClosure(f.scene, token, OwnerModalCloseReason::Unavailable);
    const auto old_first = old->Find(old->Find(f.scope)->children.front());
    assert(old_first);
    const auto old_bounds = old_first->bounds;
    const contracts::LogicalPoint old_point{old_bounds.x + old_bounds.width / 2,
                                            old_bounds.y + old_bounds.height / 2};
    Button(f.scene, old, old_point, true);
    assert(!Button(f.scene, old, old_point, false).activation);
    assert(!f.scene.EndOwnerModal(token));
}

void CheckNativePopupEpoch()
{
    Fixture f;
    const auto trigger = f.Find("outside-open");
    const auto command = f.Find("outside-popup-command");
    assert(f.scene.OpenPopup(trigger));
    f.Present();
    const auto request = f.scene.CapturePopupSurfaceRequest(4);
    assert(request);
    const PopupSurfaceConfigure configure{
        4,
        9,
        {std::floor(request->anchor.x + request->anchor.width / 2 - 100),
         std::ceil(request->anchor.y + request->anchor.height + request->gap), 200, 100}};
    const auto plan = f.scene.PreparePopupSurface(*request, configure);
    assert(plan && plan->input_snapshot->owner_modal_epoch == 0);
    const PopupSurfaceIdentity identity{3, 51, 51, 9, 1};
    assert(f.scene.AdoptPopupSurface(*plan, identity));
    f.Present();
    assert(f.scene.HasPopupSurfaceAdoption());
    const auto old_root = f.shown;
    const auto body = plan->input_snapshot->Find(command)->bounds;
    const contracts::LogicalPoint point{body.x + body.width / 2, body.y + body.height / 2};
    const contracts::PointerButtonEvent down{
        window,  point, contracts::PointerButton::Primary, contracts::ButtonState::Pressed, 0, 1,
        pointer, 731};
    auto up = down;
    up.state = contracts::ButtonState::Released;
    assert(!f.scene.HandlePopupSurfaceInput(down, identity, plan->input_snapshot).activation);
    assert(f.scene.State(command).captured);

    const auto token = f.Begin();
    assert(!f.scene.HasPopupSurfaceAdoption() && !f.scene.PopupToken());
    assert(!f.scene.State(command).captured);
    assert(!f.scene.HandlePopupSurfaceInput(up, identity, plan->input_snapshot).activation);
    assert(!f.scene.ApplyInputSnapshot(old_root));
    assert(!f.scene.IsInputSnapshotAdopted(*old_root));
    assert(!f.scene.PreparePopupSurface(*request, configure));
    const PopupSurfaceIdentity later_identity{3, 51, 51, 9, 2};
    assert(!f.scene.AdoptPopupSurface(*plan, later_identity));
    assert(!f.scene.CapturePopupSurfaceRequest(4));

    assert(f.scene.EndOwnerModal(token));
    ExpectClosure(f.scene, token, OwnerModalCloseReason::Ended);
    f.Present();
    const auto epoch = f.scene.OwnerModalEpoch();
    assert(epoch > plan->input_snapshot->owner_modal_epoch);
    assert(!f.scene.AdoptPopupSurface(*plan, later_identity));
    assert(f.scene.OpenPopup(trigger));
    f.Present();
    const auto next_request = f.scene.CapturePopupSurfaceRequest(4);
    assert(next_request);
    const auto next_plan = f.scene.PreparePopupSurface(*next_request, configure);
    assert(next_plan && next_plan->input_snapshot->owner_modal_epoch == epoch);
    const PopupSurfaceIdentity next_identity{3, 52, 52, 9, 1};
    assert(f.scene.AdoptPopupSurface(*next_plan, next_identity));
    assert(f.scene.HasPopupSurfaceAdoption());
    assert(!f.scene.HandlePopupSurfaceInput(down, identity, plan->input_snapshot).activation);
    assert(!f.scene.State(command).captured);
    assert(!f.scene.HandlePopupSurfaceInput(down, next_identity, next_plan->input_snapshot)
                .activation);
    const auto activated =
        f.scene.HandlePopupSurfaceInput(up, next_identity, next_plan->input_snapshot);
    assert(activated.activation && activated.activation->action == "outside-popup-command");
}

void CheckCleanupWithoutGeometry()
{
    constexpr contracts::InputSource slider_pointer{0, 9, 1};
    constexpr contracts::InputSource touch{0, 3, 1};
    for (unsigned geometry = 0; geometry < 3; ++geometry) {
        for (unsigned cleanup = 0; cleanup < 3; ++cleanup) {
            Fixture f;
            Fixture foreign;
            const auto before = f.shown;
            const auto token = f.Begin();
            const auto epoch = f.scene.OwnerModalEpoch();
            const auto rejected = geometry == 0   ? before
                                  : geometry == 1 ? foreign.shown
                                                  : std::shared_ptr<const InputSnapshot>{};
            const auto dragged = f.Find("inside-drag");
            const auto begin = f.Drag(dragged);
            const auto slider = f.Find("inside-value");
            f.SliderDown(slider, slider_pointer);
            const auto preview = ExpectControl(f.scene, slider, ValuePhase::Preview);
            assert(f.scene.State(dragged).dragging && f.scene.State(slider).captured);

            auto native_descriptor = std::make_shared<InputSnapshot>(*f.shown);
            native_descriptor->scene = 0;
            // A native descriptor cannot claim root lifecycle authority before
            // target adoption, even when its epoch resembles the current domain.
            f.scene.HandleInput(contracts::FocusEvent{window, false, keyboard}, native_descriptor);
            f.scene.HandleInput(contracts::CloseRequestedEvent{window}, native_descriptor);
            f.scene.HandleInput(contracts::PointerCancelEvent{window, 5, pointer},
                                native_descriptor);
            assert(f.scene.State(dragged).dragging && f.scene.State(slider).captured);
            assert(f.scene.TakeGestureEvents().empty() && f.scene.TakeControlEvents().empty());

            // A geometry-free cancellation exception must not reopen the gate for
            // presses, motion, keys or text carrying the same rejected snapshot.
            const auto first = f.Find("inside-first");
            assert(!Button(f.scene, rejected, f.Point(first), true).activation);
            assert(!Button(f.scene, rejected, f.Point(first), false).activation);
            f.scene.HandleInput(contracts::PointerMotionEvent{window, {0, 0}, 5, pointer},
                                rejected);
            const auto denied_key = f.scene.HandleInput(
                contracts::KeyEvent{
                    window, enter_key, contracts::ButtonState::Pressed, false, 5, keyboard, {}},
                rejected);
            assert(!denied_key.activation);
            assert(!f.scene.HandleInput(contracts::TextInputEvent{window, "ignored", 5}, rejected)
                        .text_edit);
            assert(f.scene.TakeGestureEvents().empty());
            assert(f.scene.State(dragged).dragging && f.scene.State(slider).captured);

            contracts::WindowEvent event;
            if (cleanup == 0) {
                event = contracts::FocusEvent{window, false, keyboard};
            } else if (cleanup == 1) {
                event = contracts::CloseRequestedEvent{window};
            } else {
                event = contracts::PointerCancelEvent{window, 6, pointer};
            }
            const auto cancelled = f.scene.HandleInput(event, rejected);
            assert(!cancelled.activation && !cancelled.control_edit && !cancelled.text_edit);
            if (cleanup == 2) {
                assert(f.scene.State(slider).captured); // A separate source is independent.
                f.scene.HandleInput(contracts::PointerCancelEvent{window, 6, slider_pointer},
                                    rejected);
            }
            ExpectGestureCancel(f.scene, begin);
            const auto control = ExpectControl(f.scene, slider, ValuePhase::Cancel);
            assert(control.event.interaction == preview.event.interaction);
            assert(control.event.value == preview.event.before);
            assert(!f.scene.State(slider).captured);
            assert(f.scene.OwnerModalToken() == token && f.scene.OwnerModalEpoch() == epoch);
            assert(f.scene.TakeOwnerModalClosures().empty());

            // Repeated cleanup and a later valid release cannot synthesize a commit.
            assert(!f.scene.HandleInput(event, rejected).activation);
            assert(f.scene.TakeGestureEvents().empty() && f.scene.TakeControlEvents().empty());
            assert(!Button(f.scene, f.shown, f.Point(dragged), false).activation);
            assert(!Button(f.scene, f.shown, f.Point(slider), false, slider_pointer).activation);
            assert(f.scene.TakeGestureEvents().empty() && f.scene.TakeControlEvents().empty());

            f.scene.HandleInput(contracts::FocusEvent{window, true, keyboard}, rejected);
            if (cleanup == 2) {
                assert(f.scene.State(slider).focused); // Pointer cancellation preserves key focus.
            } else {
                assert(f.scene.State(first).focused); // Losing key focus restores the scope entry.
            }
            const auto selected = f.Click(first);
            assert(selected.activation && selected.activation->action == "inside-first");
            f.Key(enter_key);
            assert(f.scene.State(first).pressed);
            f.scene.HandleInput(contracts::CloseRequestedEvent{window}, rejected);
            assert(!f.scene.State(first).pressed && !f.Key(enter_key, false).activation);

            f.scene.HandleInput(contracts::TouchDownEvent{window, f.Point(first), 1, 7, touch},
                                f.shown);
            assert(f.scene.State(first).captured);
            const auto touch_cancel =
                f.scene.HandleInput(contracts::TouchCancelEvent{window, 8, touch}, rejected);
            assert(!touch_cancel.activation && !f.scene.State(first).captured);
            assert(!f.scene.HandleInput(contracts::TouchUpEvent{window, 1, 9, touch}, f.shown)
                        .activation);
            assert(f.scene.OwnerModalToken() == token && f.scene.OwnerModalEpoch() == epoch);
            assert(f.scene.EndOwnerModal(token));
            ExpectClosure(f.scene, token, OwnerModalCloseReason::Ended);
        }
    }
}

} // namespace

int main()
{
    CheckSnapshotAdoption();
    CheckScopeAndSnapshots();
    CheckKeyboardAndText();
    CheckAllSeatsAndFocusLoss();
    CheckCaptureCancellation();
    CheckAvailabilityAndRestoration();
    CheckTransactionsAndPopupExclusion();
    CheckNativePopupEpoch();
    CheckCleanupWithoutGeometry();
}
