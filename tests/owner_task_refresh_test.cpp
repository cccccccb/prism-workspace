#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/runtime/task_session.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

using namespace prism;
using namespace prism::runtime;

namespace {
constexpr contracts::WindowId window{1};
constexpr contracts::InputSource mouse{0, 1, 1};
constexpr contracts::InputSource keyboard{0, 2, 1};
constexpr std::uint32_t enter_key = 0x28;

ShapedText Shape(std::string_view text, double font)
{
    return {{}, text.size() * 7.0, font};
}

Blueprint Layout()
{
    return ParseBlueprint(R"(
HStack(spacing:16) {
    VStack(width:160, spacing:0) {
        Button("Owner action", action:"owner-action", height:40)
        TextField($owner_text, action:"owner-edit", height:40)
    }
    VStack(width:240, spacing:0) {
        Button($row_title, action:"select-row", height:40)
        Button("Second row", action:"second-row", height:40)
        TextField($name, action:"edit-name", height:40)
        Slider(action:"slider", value:$amount, height:44) {
            Visual(sliderPart:"track", height:4)
            Visual(sliderPart:"fill", height:4)
            Visual(sliderPart:"thumb", width:16, height:16)
        }
        InteractionTarget(action:"drag-target", height:40) {
            Visual(background:#456789FF)
        }.gesture(action:"drag", threshold:6)
    }
}
)");
}

InteractionResult Button(Scene &scene, const std::shared_ptr<const InputSnapshot> &snapshot,
                         contracts::LogicalPoint point, bool down,
                         contracts::InputSource source = mouse)
{
    return scene.HandleInput(contracts::PointerButtonEvent{window, point,
                                                           contracts::PointerButton::Primary,
                                                           down ? contracts::ButtonState::Pressed
                                                                : contracts::ButtonState::Released,
                                                           0, 1, source, 42},
                             snapshot);
}

InteractionResult Key(Scene &scene, const std::shared_ptr<const InputSnapshot> &snapshot, bool down,
                      contracts::InputSource source = keyboard)
{
    return scene.HandleInput(contracts::KeyEvent{window, enter_key,
                                                 down ? contracts::ButtonState::Pressed
                                                      : contracts::ButtonState::Released,
                                                 false, 2, source},
                             snapshot);
}

struct Fixture {
    Scene scene{Layout(), Shape};
    std::shared_ptr<const InputSnapshot> shown;
    contracts::NodeId scope;

    Fixture()
    {
        scene.SetBinding("owner_text", std::string("owner"));
        scene.SetBinding("name", std::string("notes.md"));
        scene.SetBinding("row_title", std::string("First directory entry"));
        scene.SetBinding("amount", .2);
        assert(scene.SetViewport({440, 280}));
        Present();
        scope = shown->Find(Find("select-row"))->parent;
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

    InteractionResult Click(contracts::NodeId id, contracts::InputSource source = mouse)
    {
        Button(scene, shown, Point(id), true, source);
        return Button(scene, shown, Point(id), false, source);
    }

    std::uint64_t Begin()
    {
        const auto token = scene.BeginOwnerModal(scope);
        assert(token && scene.OwnerModalToken() == *token);
        Present();
        return *token;
    }
};

void CheckCoreReprepareAndTerminals()
{
    TaskSession session(IssueTaskOwnerId());
    const std::array phases{TaskPhase::Preparing, TaskPhase::Ready, TaskPhase::Working};
    std::uint64_t previous{};
    for (const auto phase : phases) {
        const auto identity = session.Begin();
        assert(identity && identity->request.value > previous);
        if (phase != TaskPhase::Preparing) {
            assert(session.SetReady(*identity));
        }
        if (phase == TaskPhase::Working) {
            assert(session.SetWorking(*identity));
        }
        const auto before = session.Active();
        const std::array invalid{TaskIdentity{}, TaskIdentity{{}, identity->request},
                                 TaskIdentity{identity->owner, {}},
                                 TaskIdentity{IssueTaskOwnerId(), identity->request},
                                 TaskIdentity{identity->owner, {identity->request.value + 1}}};
        for (const auto foreign : invalid) {
            assert(!session.Reprepare(foreign) && session.Active() == before);
        }

        assert(session.Reprepare(*identity));
        assert(session.Active() == (TaskEntry{*identity, TaskPhase::Preparing}));
        assert(session.Reprepare(*identity));
        assert(!session.Begin() && !session.SetWorking(*identity) && !session.Succeed(*identity));
        assert(session.SetReady(*identity) && session.Succeed(*identity));
        assert(!session.Reprepare(*identity));
        assert(!session.Cancel(*identity, TaskCancelReason::User));
        assert(!session.Fail(*identity, {TaskFailureCode::PreparationFailed, "late refresh"}));
        const auto terminal = session.TakeTerminal();
        assert(terminal && terminal->identity == *identity &&
               terminal->outcome == TaskOutcome::Success);
        assert(!session.TakeTerminal());
        previous = identity->request.value;
    }

    const auto cancelled = session.Begin();
    assert(cancelled && session.SetReady(*cancelled));
    assert(session.Cancel(*cancelled, TaskCancelReason::Escape));
    assert(!session.Reprepare(*cancelled) && !session.Begin());
    const auto cancellation = session.TakeTerminal();
    assert(cancellation && cancellation->identity == *cancelled &&
           cancellation->cancel_reason == TaskCancelReason::Escape);

    const auto next = session.Begin();
    assert(next && next->request.value > cancelled->request.value);
    assert(!session.Reprepare(*cancelled) && session.Active()->identity == *next);
    assert(session.Fail(*next, {TaskFailureCode::OperationFailed, "accepted failure"}));
    assert(!session.Reprepare(*next));
    session.RetireOwner();
    const auto failure = session.TakeTerminal();
    assert(failure && failure->identity == *next && failure->outcome == TaskOutcome::Failed &&
           failure->failure->diagnostic == "accepted failure");
    assert(!session.Reprepare(*next) && !session.Begin());

    TaskSession retired(IssueTaskOwnerId());
    const auto retiring = retired.Begin();
    assert(retiring && retired.SetReady(*retiring));
    retired.RetireOwner();
    assert(!retired.Reprepare(*retiring));
    assert(retired.TakeTerminal()->cancel_reason == TaskCancelReason::OwnerClosed);
    assert(!retired.Reprepare(*retiring) && !retired.Begin());
}

void CheckChangedRowRejectsOldInput()
{
    Fixture f;
    Fixture foreign;
    const auto token = f.Begin();
    const auto row = f.Find("select-row");
    const auto point = f.Point(row);
    const auto old = f.shown;
    Button(f.scene, old, point, true);
    assert(f.scene.State(row).captured);

    // Stable action and node identity now refer to a different directory entry.
    // Geometry equality must not permit an old release to select the new entry.
    assert(f.scene.SetBinding("row_title", std::string("Replacement directory entry")));
    f.scene.ResolveLayout();
    const auto refreshed = f.scene.RefreshOwnerModal(token);
    assert(refreshed && *refreshed > token);
    assert(!f.scene.State(row).captured && f.scene.TakeOwnerModalClosures().empty());
    assert(!f.scene.IsInputSnapshotAdopted(*old));
    assert(!Button(f.scene, old, point, false).activation);

    const std::array<std::shared_ptr<const InputSnapshot>, 3> rejected{old, foreign.shown, nullptr};
    for (const auto &snapshot : rejected) {
        assert(!f.scene.ApplyInputSnapshot(snapshot));
        Button(f.scene, snapshot, point, true);
        assert(!Button(f.scene, snapshot, point, false).activation);
        assert(!f.scene.State(row).captured);
        assert(!f.scene.HandleInput(contracts::TextInputEvent{window, "stale", 3}, snapshot)
                    .text_edit);
        Key(f.scene, snapshot, true);
        assert(!Key(f.scene, snapshot, false).activation);
        assert(f.scene.OwnerModalToken() == *refreshed);
    }

    f.Present();
    assert(f.shown->owner_modal_epoch == *refreshed && f.shown->version > old->version);
    assert(f.shown->Find(row)->action == old->Find(row)->action);
    assert(!f.scene.ApplyInputSnapshot(old) && f.scene.IsInputSnapshotAdopted(*f.shown));
    assert(f.Click(row).activation->action == "select-row");
    assert(!f.Click(f.Find("owner-action")).activation);
    assert(f.scene.TakeOwnerModalClosures().empty());
}

void CheckTextFocusAndOriginalReturnFocus()
{
    Fixture f;
    const auto owner_editor = f.Find("owner-edit");
    f.Click(owner_editor);
    assert(f.scene.State(owner_editor).focused);
    const auto token = f.Begin();
    const auto editor = f.Find("edit-name");
    f.Click(editor);
    assert(f.scene.State(editor).focused && f.scene.FocusedAction() == "edit-name");
    const auto old = f.shown;

    assert(f.scene.SetBinding("row_title", std::string("Another directory")));
    f.scene.ResolveLayout();
    const auto refreshed = f.scene.RefreshOwnerModal(token);
    assert(refreshed && f.scene.State(editor).focused);
    assert(f.scene.FocusedAction() == "edit-name" && !f.scene.State(owner_editor).focused);
    assert(!f.scene.HandleInput(contracts::TextInputEvent{window, "ignored", 4}, old).text_edit);
    assert(!f.scene.EndOwnerModal(token) && f.scene.TakeOwnerModalClosures().empty());

    const auto repeated = f.scene.RefreshOwnerModal(*refreshed);
    assert(repeated && *repeated > *refreshed && f.scene.State(editor).focused);
    assert(f.scene.FocusedAction() == "edit-name" && !f.scene.State(owner_editor).focused);
    assert(!f.scene.EndOwnerModal(*refreshed) && f.scene.TakeOwnerModalClosures().empty());

    f.Present();
    const auto typed = f.scene.HandleInput(contracts::TextInputEvent{window, "X", 5}, f.shown);
    assert(typed.text_edit && typed.text_edit->action == "edit-name" &&
           typed.text_edit->text.find('X') != std::string::npos);
    assert(f.scene.State(editor).focused);

    assert(f.scene.EndOwnerModal(*repeated));
    assert(f.scene.State(owner_editor).focused && !f.scene.State(editor).focused);
    const auto closures = f.scene.TakeOwnerModalClosures();
    assert(closures.size() == 1 && closures.front().token == *repeated &&
           closures.front().reason == OwnerModalCloseReason::Ended);
}

void CheckTextFollowsItsKeyboardSeat()
{
    constexpr contracts::InputSource native_mouse{1, 1, 1};
    constexpr contracts::InputSource native_keyboard{1, 2, 1};
    constexpr contracts::InputSource other_keyboard{2, 2, 1};
    Fixture f;
    const auto token = f.Begin();
    const auto first_focus = f.Find("select-row");
    const auto editor = f.Find("edit-name");
    assert(f.scene.State(first_focus).focused && f.scene.FocusedAction() == "select-row");
    f.Click(editor, native_mouse);
    assert(f.scene.State(first_focus).focused && f.scene.State(editor).focused);

    contracts::KeyEvent select_all{window,
                                   0x04,
                                   contracts::ButtonState::Pressed,
                                   false,
                                   6,
                                   native_keyboard,
                                   {false, true, false, false}};
    const auto selected = f.scene.HandleInput(select_all, f.shown);
    assert(selected.changed && !selected.text_edit);
    select_all.state = contracts::ButtonState::Released;
    f.scene.HandleInput(select_all, f.shown);
    const auto typed =
        f.scene.HandleInput(contracts::TextInputEvent{window, "n", 7, native_keyboard}, f.shown);
    assert(typed.text_edit && typed.text_edit->action == "edit-name" &&
           typed.text_edit->text == "n"); // Ctrl+A and UTF-8 must reach the same editor.
    assert(f.scene.FocusedAction() == "select-row");
    assert(
        !f.scene.HandleInput(contracts::TextInputEvent{window, "seat zero", 8}, f.shown).text_edit);
    assert(
        !f.scene
             .HandleInput(contracts::TextInputEvent{window, "no focus", 9, other_keyboard}, f.shown)
             .text_edit);

    const auto old = f.shown;
    f.scene.ResolveLayout();
    const auto refreshed = f.scene.RefreshOwnerModal(token);
    assert(refreshed && f.scene.State(editor).focused && f.scene.State(first_focus).focused);
    assert(
        !f.scene.HandleInput(contracts::TextInputEvent{window, "stale", 10, native_keyboard}, old)
             .text_edit);
    f.Present();
    select_all.state = contracts::ButtonState::Pressed;
    f.scene.HandleInput(select_all, f.shown);
    select_all.state = contracts::ButtonState::Released;
    f.scene.HandleInput(select_all, f.shown);
    const auto fresh = f.scene.HandleInput(
        contracts::TextInputEvent{window, "new.md", 11, native_keyboard}, f.shown);
    assert(fresh.text_edit && fresh.text_edit->action == "edit-name" &&
           fresh.text_edit->text == "new.md");

    f.scene.HandleInput(contracts::FocusEvent{window, false, native_keyboard}, f.shown);
    assert(!f.scene.State(editor).focused && f.scene.State(first_focus).focused);
    assert(!f.scene
                .HandleInput(contracts::TextInputEvent{window, "after loss", 12, native_keyboard},
                             f.shown)
                .text_edit);
    assert(f.scene.EndOwnerModal(*refreshed));
}

void CheckRefreshCancelsCapturedStreams()
{
    constexpr contracts::InputSource key_pointer{1, 1, 1};
    constexpr contracts::InputSource key_source{1, 2, 1};
    constexpr contracts::InputSource drag_pointer{2, 1, 1};
    constexpr contracts::InputSource slider_pointer{3, 1, 1};
    constexpr contracts::InputSource touch{4, 1, 1};
    constexpr contracts::InputSource held_pointer{5, 1, 1};
    Fixture f;
    const auto token = f.Begin();
    const auto row = f.Find("select-row");
    const auto second = f.Find("second-row");
    const auto slider = f.Find("slider");
    const auto dragged = f.Find("drag-target");
    f.Click(row, key_pointer);
    Key(f.scene, f.shown, true, key_source);
    assert(f.scene.State(row).pressed);
    Button(f.scene, f.shown, f.Point(row), true, held_pointer);

    const auto point = f.Point(dragged);
    Button(f.scene, f.shown, point, true, drag_pointer);
    f.scene.HandleInput(
        contracts::PointerMotionEvent{window, {point.x + 10, point.y}, 6, drag_pointer}, f.shown);
    const auto begin = f.scene.TakeGestureEvents();
    assert(begin.size() == 1 && begin.front().phase == contracts::GesturePhase::Begin &&
           f.scene.State(dragged).dragging);

    const auto track = f.shown->Find(slider)->slider_track;
    assert(track.width > 0 && track.height > 0);
    const contracts::LogicalPoint slider_point{track.x + track.width * .8, track.y};
    const auto slider_hit = f.scene.HitTest(slider_point, *f.shown);
    assert(slider_hit && slider_hit->node == slider);

    Button(f.scene, f.shown, slider_point, true, slider_pointer);
    const auto preview = f.scene.TakeControlEvents();
    assert(preview.size() == 1 && preview.front().event.phase == ValuePhase::Preview);
    assert(f.scene.State(slider).captured);
    f.scene.HandleInput(contracts::TouchDownEvent{window, f.Point(second), 1, 7, touch}, f.shown);
    assert(f.scene.State(second).captured);
    const auto old = f.shown;

    f.scene.ResolveLayout();
    const auto refreshed = f.scene.RefreshOwnerModal(token);
    assert(refreshed && f.scene.TakeOwnerModalClosures().empty());
    assert(!f.scene.State(row).pressed && !f.scene.State(row).captured &&
           !f.scene.State(second).captured && !f.scene.State(slider).captured &&
           !f.scene.State(dragged).dragging && !f.scene.State(dragged).captured);
    const auto cancelled = f.scene.TakeGestureEvents();
    assert(cancelled.size() == 1 && cancelled.front().id == begin.front().id &&
           cancelled.front().phase == contracts::GesturePhase::Cancel);
    const auto controls = f.scene.TakeControlEvents();
    assert(controls.size() == 1 &&
           controls.front().event.interaction == preview.front().event.interaction &&
           controls.front().event.phase == ValuePhase::Cancel);

    assert(!Key(f.scene, old, false, key_source).activation);
    assert(!Button(f.scene, old, f.Point(row), false, held_pointer).activation);
    assert(!f.scene.HandleInput(contracts::TouchUpEvent{window, 1, 8, touch}, old).activation);
    assert(f.scene.TakeGestureEvents().empty() && f.scene.TakeControlEvents().empty());
    f.Present();
    assert(!Key(f.scene, f.shown, false, key_source).activation);
    assert(!Button(f.scene, f.shown, f.Point(row), false, held_pointer).activation);
    assert(!f.scene.HandleInput(contracts::TouchUpEvent{window, 1, 9, touch}, f.shown).activation);
    f.Click(row, key_pointer);
    Key(f.scene, f.shown, true, key_source);
    const auto activated = Key(f.scene, f.shown, false, key_source);
    assert(activated.activation && activated.activation->action == "select-row");
}

void CheckRefreshPreconditionsAndEpochLimit()
{
    Fixture f;
    assert(!f.scene.RefreshOwnerModal(0) && !f.scene.RefreshOwnerModal(1));
    const auto token = f.Begin();
    const auto epoch = f.scene.OwnerModalEpoch();
    const auto row = f.Find("select-row");
    Button(f.scene, f.shown, f.Point(row), true);
    assert(!f.scene.RefreshOwnerModal(0) && !f.scene.RefreshOwnerModal(token + 1));
    assert(f.scene.OwnerModalToken() == token && f.scene.OwnerModalEpoch() == epoch &&
           f.scene.State(row).captured);

    assert(f.scene.SetViewport({460, 280}));
    assert(!f.scene.RefreshOwnerModal(token));
    assert(f.scene.OwnerModalToken() == token && f.scene.OwnerModalEpoch() == epoch);
    assert(f.scene.TakeOwnerModalClosures().empty());
    f.scene.ResolveLayout();
    const auto refreshed = f.scene.RefreshOwnerModal(token);
    assert(refreshed && *refreshed > token);
    assert(f.scene.EndOwnerModal(*refreshed));
    const auto ended_epoch = f.scene.OwnerModalEpoch();
    assert(!f.scene.RefreshOwnerModal(*refreshed) && f.scene.OwnerModalEpoch() == ended_epoch);

    Fixture exhausted;
    exhausted.scene.owner_modal_epoch_ = std::numeric_limits<std::uint64_t>::max() - 2;
    const auto final_token = exhausted.Begin();
    assert(final_token == std::numeric_limits<std::uint64_t>::max() - 1);
    const auto before = exhausted.shown;
    assert(!exhausted.scene.RefreshOwnerModal(final_token));
    assert(exhausted.scene.OwnerModalToken() == final_token &&
           exhausted.scene.IsInputSnapshotAdopted(*before));
    assert(exhausted.scene.TakeOwnerModalClosures().empty());
    assert(exhausted.scene.EndOwnerModal(final_token));
    assert(exhausted.scene.OwnerModalEpoch() == std::numeric_limits<std::uint64_t>::max());
    assert(!exhausted.scene.BeginOwnerModal(exhausted.scope));
}
} // namespace

int main()
{
    CheckCoreReprepareAndTerminals();
    CheckChangedRowRejectsOldInput();
    CheckTextFocusAndOriginalReturnFocus();
    CheckTextFollowsItsKeyboardSeat();
    CheckRefreshCancelsCapturedStreams();
    CheckRefreshPreconditionsAndEpochLimit();
}
