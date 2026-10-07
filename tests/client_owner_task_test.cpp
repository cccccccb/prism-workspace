#include "client_application_p.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using namespace prism;

namespace {
constexpr contracts::WindowId window{1};
constexpr contracts::InputSource pointer{0, 1, 1};
constexpr contracts::InputSource keyboard{0, 2, 1};

runtime::ShapedText Shape(std::string_view text, double font)
{
    return {{}, text.size() * 7.0, font};
}

runtime::Blueprint Layout()
{
    auto blueprint = runtime::ParseBlueprint(R"(
HStack(spacing:20) {
    VStack(width:180, spacing:8) {
        Button("Document", action:"outside", height:40)
        Text("Work stays in this owner", height:32)
        InteractionTarget(action:"outside-drag", height:40) {
            Visual(background:#123456FF)
        }.gesture(action:"outside-gesture", threshold:6)
    }
    Card(width:220, visible:$task_visible, enabled:$task_enabled) {
        VStack(spacing:8) {
            Text("Task", height:32)
            Button("Accept", action:"inside", height:40)
        }
    }
    Card(width:20) {}
}
)");
    blueprint.children.at(1).region = "task";
    blueprint.children.at(1).region_mounted = true;
    blueprint.children.at(2).region = "pending";
    return blueprint;
}

std::unique_ptr<runtime::Scene> MakeScene()
{
    auto scene = std::make_unique<runtime::Scene>(Layout(), Shape);
    const auto scope = scene->RegionId("task");
    assert(scope && scene->RegionMounted("task"));
    assert(scene->AcceptsBinding("task_visible", true));
    assert(scene->AcceptsBinding("task_enabled", true));

    // Scene::SetBinding reports a change, not validity. Both defaults are true;
    // exercise a real round trip and inspect the mounted wrapper's live state.
    assert(scene->SetBinding("task_visible", false));
    assert(!scene->IsVisible(scope));
    assert(scene->SetBinding("task_visible", true));
    assert(scene->IsVisible(scope));
    assert(scene->SetBinding("task_enabled", false));
    assert(!scene->State(scope).enabled);
    assert(scene->SetBinding("task_enabled", true));
    assert(scene->State(scope).enabled);

    assert(scene->SetViewport({480, 220}));
    assert(scene->Build(window));
    scene->AcknowledgeComposite();
    return scene;
}

sdk::ClientConfig Config()
{
    sdk::ClientConfig config;
    config.app_id = "owner-task-fixture";
    config.width = 480;
    config.height = 220;
    return config;
}

// The fixture opens no native connection, renderer worker or GPU. Region/Scene
// adoption is exercised with real immutable Scene snapshots and SDK policy;
// simulated successful metadata is not a real compositor submission test.
struct Fixture {
    sdk::ClientApplication app{Config()};
    std::shared_ptr<const runtime::InputSnapshot> shown;
    std::uint64_t metadata_sequence{};

    Fixture()
    {
        auto &impl = *app.impl_;
        impl.scene = MakeScene();
        impl.opened_once = true;
        impl.installed_ui = impl.ui_load.Begin();
        impl.binding_values = {{"task_visible", true}, {"task_enabled", true}};
        assert(!impl.render_owner && !impl.worker_generation);
        assert(impl.ui_configure_count == 0);
        shown = impl.scene->InputGeometry();
        impl.scene->ApplyInputSnapshot(shown);
        assert(impl.scene->IsInputSnapshotAdopted(*shown));
    }

    runtime::Scene &Scene()
    {
        assert(app.impl_->scene);
        return *app.impl_->scene;
    }

    std::shared_ptr<const runtime::InputSnapshot> Capture()
    {
        Scene().Build(window);
        return Scene().CaptureInputSnapshot();
    }

    runtime::TaskIdentity Begin()
    {
        const auto identity = app.BeginOwnerTask("task");
        assert(identity);
        assert(app.ActiveOwnerTask() ==
               (runtime::TaskEntry{*identity, runtime::TaskPhase::Preparing}));
        assert(app.impl_->owner_task_scope);
        assert(app.impl_->owner_task_scope->identity == *identity);
        assert(app.impl_->owner_task_scope->ui == app.impl_->installed_ui);
        assert(app.impl_->owner_task_scope->token == Scene().OwnerModalToken());
        assert(Scene().OwnerModalToken());
        return *identity;
    }

    void Adopt(const std::shared_ptr<const runtime::InputSnapshot> &input, runtime::UiLoadId ui)
    {
        // Apply first, then model the Host's ordered successful metadata handoff.
        // Apply's bool indicates changed pixels, not whether geometry was adopted.
        Scene().ApplyInputSnapshot(input);
        app.impl_->AdoptOwnerTaskInput(input, ui);
    }

    void Ready(runtime::TaskIdentity identity)
    {
        shown = Capture();
        Adopt(shown, app.impl_->installed_ui);
        assert(Scene().IsInputSnapshotAdopted(*shown));
        assert(app.ActiveOwnerTask() == (runtime::TaskEntry{identity, runtime::TaskPhase::Ready}));
    }

    void SubmitMetadata(const std::shared_ptr<const runtime::InputSnapshot> &input,
                        runtime::UiLoadId ui, bool metadata_prepared)
    {
        auto frame = std::make_shared<runtime::FramePacket>();
        frame->ui = ui;
        frame->sequence = ++metadata_sequence;
        frame->scene_revision = Scene().TransactionRevision();
        frame->pixels_revision = Scene().PixelsRevision();
        frame->theme_generation = app.ThemeGeneration();
        frame->input_snapshot = input;

        runtime::SubmittedFrameEvent event;
        event.ui = ui;
        event.frame_sequence = frame->sequence;
        event.frame = std::move(frame);
        event.scene_revision = event.frame->scene_revision;
        event.pixels_revision = event.frame->pixels_revision;
        event.theme_generation = event.frame->theme_generation;
        event.metadata_prepared = metadata_prepared;
        app.impl_->HandleSubmitted(event);
        assert(!app.impl_->failed);
    }

    runtime::TaskTerminal Take(runtime::TaskIdentity identity, runtime::TaskOutcome outcome)
    {
        assert(!app.ActiveOwnerTask());
        assert(!app.impl_->owner_task_scope);
        assert(!Scene().OwnerModalToken());
        const auto terminal = app.TakeOwnerTaskTerminal();
        assert(terminal && terminal->identity == identity && terminal->outcome == outcome);
        assert(!app.TakeOwnerTaskTerminal());
        return *terminal;
    }

    runtime::InteractionResult Click(std::string_view action)
    {
        const runtime::InputSnapshotNode *target = nullptr;
        for (const auto &node : shown->nodes) {
            if (node.action == action) {
                target = &node;
                break;
            }
        }
        assert(target);
        const auto bounds = target->bounds;
        const contracts::LogicalPoint point{bounds.x + bounds.width / 2,
                                            bounds.y + bounds.height / 2};
        const contracts::PointerButtonEvent down{window,
                                                 point,
                                                 contracts::PointerButton::Primary,
                                                 contracts::ButtonState::Pressed,
                                                 0,
                                                 1,
                                                 pointer,
                                                 781};
        auto up = down;
        up.state = contracts::ButtonState::Released;
        Scene().HandleInput(down, shown);
        return Scene().HandleInput(up, shown);
    }
};

void CheckBeginEligibility()
{
    sdk::ClientApplication unopened(Config());
    assert(!unopened.BeginOwnerTask("task"));
    assert(!unopened.ActiveOwnerTask() && !unopened.impl_->owner_tasks);

    Fixture f;
    assert(!f.app.BeginOwnerTask(""));
    assert(!f.app.BeginOwnerTask("unknown"));
    assert(!f.app.BeginOwnerTask("pending"));
    assert(!f.app.ActiveOwnerTask());
    assert(!f.Scene().OwnerModalToken());
    assert(!f.app.TakeOwnerTaskTerminal());

    assert(f.app.SetBinding("task_visible", false));
    f.Capture();
    assert(!f.app.BeginOwnerTask("task"));
    assert(f.app.SetBinding("task_visible", true));
    assert(f.app.SetBinding("task_enabled", false));
    f.Capture();
    assert(!f.app.BeginOwnerTask("task"));
    assert(f.app.SetBinding("task_enabled", true));
    f.Capture();
    const auto identity = f.Begin();
    assert(!f.app.BeginOwnerTask("task"));
    assert(f.app.CancelOwnerTask(identity));
    const auto terminal = f.Take(identity, runtime::TaskOutcome::Cancelled);
    assert(terminal.cancel_reason == runtime::TaskCancelReason::User);
}

void FillCommandQueue(runtime::PollableQueue<runtime::RenderCommand> &queue)
{
    while (queue.Size() < queue.Capacity()) {
        runtime::RenderCommand command(
            runtime::RequestRenderCommand{runtime::RenderRequestKind::Update, false});
        assert(queue.TryPush(std::move(command)) == runtime::QueuePushResult::Accepted);
    }

    runtime::RenderCommand overflow(
        runtime::RequestRenderCommand{runtime::RenderRequestKind::Update, false});
    assert(queue.TryPush(std::move(overflow)) == runtime::QueuePushResult::Busy);
    assert(queue.Size() == queue.Capacity() && !queue.IsClosed());
}

void CheckPublicationFailureRollsBack()
{
    for (const bool configured : std::array{false, true}) {
        Fixture f;
        auto &impl = *f.app.impl_;
        if (configured) {
            // Only recorded configuration metadata is needed to exercise the
            // genuine FrameCommand path; no native window or worker is opened.
            impl.ui_configure_count = 1;
            impl.ui_metrics = {{480, 220}, {480, 220}, 1};
        }
        FillCommandQueue(impl.bridge->render_commands);
        assert(impl.bridge->terminal.Reason() == runtime::TerminalReason::None);

        bool threw = false;
        std::optional<runtime::TaskIdentity> returned;
        try {
            returned = f.app.BeginOwnerTask("task");
        } catch (const std::runtime_error &) {
            threw = true;
        }

        // Without configure, the Update control request fails. With configure,
        // publication fails to enqueue the freshly captured immutable frame.
        // Neither failed path may strand a task that the caller never received.
        assert(threw && !returned);
        assert(impl.bridge->terminal.Reason() == runtime::TerminalReason::CommandQueueFailure);
        assert(impl.bridge->render_commands.Size() == impl.bridge->render_commands.Capacity());
        assert(!f.app.ActiveOwnerTask());
        assert(impl.owner_tasks && !impl.owner_tasks->Active());
        assert(!impl.owner_task_scope);
        assert(!f.Scene().OwnerModalToken());
        assert(!f.app.TakeOwnerTaskTerminal());
        assert(!impl.render_owner && !impl.worker_generation);
    }
}

void CheckUnresolvedLayoutCanRetry()
{
    Fixture f;
    const auto epoch = f.Scene().OwnerModalEpoch();
    assert(f.Scene().SetViewport({520, 260}));
    assert(runtime::Has(f.Scene().PendingDirty(), runtime::Dirty::Layout));
    assert(!f.app.BeginOwnerTask("task"));
    assert(!f.app.ActiveOwnerTask());
    assert(!f.app.impl_->owner_task_scope);
    assert(!f.Scene().OwnerModalToken() && f.Scene().OwnerModalEpoch() == epoch);
    assert(!f.app.TakeOwnerTaskTerminal());

    f.Capture();
    assert(!runtime::Has(f.Scene().PendingDirty(), runtime::Dirty::Layout));
    const auto identity = f.Begin();
    f.Ready(identity);
    assert(f.app.CompleteOwnerTask(identity));
    f.Take(identity, runtime::TaskOutcome::Success);
}

void CheckQueuedAndSuccessfulMetadata()
{
    Fixture f;
    const auto identity = f.Begin();
    const auto queued = f.Capture();
    assert(queued->owner_modal_epoch == f.Scene().OwnerModalToken());
    assert(!f.Scene().IsInputSnapshotAdopted(*queued));
    assert(!f.app.SetOwnerTaskWorking(identity));
    assert(!f.app.ResumeOwnerTask(identity));
    assert(!f.app.CompleteOwnerTask(identity));
    assert(f.app.ActiveOwnerTask()->phase == runtime::TaskPhase::Preparing);

    f.SubmitMetadata(queued, f.app.impl_->installed_ui, false);
    assert(!f.Scene().IsInputSnapshotAdopted(*queued));
    assert(f.app.ActiveOwnerTask()->phase == runtime::TaskPhase::Preparing);

    // With no pointer interaction, input adoption changes no pixels. The SDK
    // must still mark Ready instead of treating Apply's false as rejection.
    const auto pixels = f.Scene().PixelsRevision();
    f.SubmitMetadata(queued, f.app.impl_->installed_ui, true);
    assert(f.Scene().IsInputSnapshotAdopted(*queued));
    assert(f.Scene().PixelsRevision() == pixels);
    assert(f.app.ActiveOwnerTask()->phase == runtime::TaskPhase::Ready);
    assert(f.app.CompleteOwnerTask(identity));
    const auto terminal = f.Take(identity, runtime::TaskOutcome::Success);
    assert(!terminal.cancel_reason && !terminal.failure);
}

void CheckOldEpochAndForeignUi()
{
    Fixture f;
    Fixture other;
    const auto old = f.shown;
    const auto identity = f.Begin();
    f.Adopt(old, f.app.impl_->installed_ui);
    assert(!f.Scene().IsInputSnapshotAdopted(*old));
    f.Adopt(other.shown, f.app.impl_->installed_ui);
    assert(!f.Scene().IsInputSnapshotAdopted(*other.shown));
    assert(f.app.ActiveOwnerTask()->phase == runtime::TaskPhase::Preparing);

    const auto candidate = f.Capture();
    f.Adopt(candidate, other.app.impl_->installed_ui);
    assert(f.Scene().IsInputSnapshotAdopted(*candidate));
    assert(f.app.ActiveOwnerTask()->phase == runtime::TaskPhase::Preparing);
    const auto loading = f.app.BeginUiLoad();
    assert(loading != f.app.impl_->installed_ui);
    f.Adopt(candidate, loading);
    assert(f.app.ActiveOwnerTask()->phase == runtime::TaskPhase::Preparing);
    f.Adopt(candidate, f.app.impl_->installed_ui);
    assert(f.app.ActiveOwnerTask()->phase == runtime::TaskPhase::Ready);
    assert(f.app.CancelOwnerTask(identity, runtime::TaskCancelReason::Escape));
    f.Take(identity, runtime::TaskOutcome::Cancelled);

    const auto next = f.Begin();
    assert(next.owner == identity.owner && next.request.value > identity.request.value);
    assert(f.Scene().OwnerModalToken() > candidate->owner_modal_epoch);
    f.Adopt(candidate, f.app.impl_->installed_ui);
    assert(f.app.ActiveOwnerTask()->phase == runtime::TaskPhase::Preparing);
    f.Ready(next);
    assert(!f.app.CompleteOwnerTask(identity));
    assert(!f.app.CancelOwnerTask(identity));
    assert(!f.app.FailOwnerTask(identity, {runtime::TaskFailureCode::OperationFailed, "late"}));
    assert(f.app.ActiveOwnerTask()->identity == next);
    assert(f.app.CompleteOwnerTask(next));
    f.Take(next, runtime::TaskOutcome::Success);
}

void CheckWorkingRecoveryAndFailure()
{
    Fixture f;
    const auto identity = f.Begin();
    f.Ready(identity);
    const auto token = f.Scene().OwnerModalToken();
    assert(!f.app.ResumeOwnerTask(identity));
    assert(f.app.SetOwnerTaskWorking(identity));
    assert(!f.app.SetOwnerTaskWorking(identity));
    assert(f.app.ResumeOwnerTask(identity));
    assert(!f.app.ResumeOwnerTask(identity));
    assert(f.app.ActiveOwnerTask()->phase == runtime::TaskPhase::Ready);
    assert(f.Scene().OwnerModalToken() == token);
    assert(f.app.SetOwnerTaskWorking(identity));

    const auto active = f.app.ActiveOwnerTask();
    assert(!f.app.FailOwnerTask(identity, {static_cast<runtime::TaskFailureCode>(99), "bad"}));
    assert(
        !f.app.FailOwnerTask(identity, {runtime::TaskFailureCode::OperationFailed,
                                        std::string(runtime::kMaxTaskDiagnosticBytes + 1, 'x')}));
    assert(f.app.ActiveOwnerTask() == active && f.Scene().OwnerModalToken() == token);

    const runtime::TaskFailure failure{runtime::TaskFailureCode::OperationFailed,
                                       "Provider could not finish"};
    assert(f.app.FailOwnerTask(identity, failure));
    assert(!f.app.BeginOwnerTask("task"));
    assert(!f.app.CompleteOwnerTask(identity));
    const auto terminal = f.Take(identity, runtime::TaskOutcome::Failed);
    assert(terminal.failure == failure && !terminal.cancel_reason);
}

void CheckEscapeAndUnavailableScope()
{
    Fixture f;
    const auto identity = f.Begin();
    f.Ready(identity);
    f.Scene().HandleInput(
        contracts::KeyEvent{window, 0x29, contracts::ButtonState::Pressed, false, 3, keyboard, {}},
        f.shown);
    f.app.impl_->ReconcileOwnerTask();
    const auto terminal = f.Take(identity, runtime::TaskOutcome::Cancelled);
    assert(terminal.cancel_reason == runtime::TaskCancelReason::Escape);

    const std::array phases{runtime::TaskPhase::Preparing, runtime::TaskPhase::Ready,
                            runtime::TaskPhase::Working};
    for (const auto phase : phases) {
        Fixture hidden;
        const auto task = hidden.Begin();
        if (phase != runtime::TaskPhase::Preparing) {
            hidden.Ready(task);
        }
        if (phase == runtime::TaskPhase::Working) {
            assert(hidden.app.SetOwnerTaskWorking(task));
        }
        assert(hidden.app.SetBinding("task_visible", false));
        // SetBinding's real SDK path must reconcile the closure; callers do not
        // manually reconcile to conceal a missing production integration point.
        const auto cancelled = hidden.Take(task, runtime::TaskOutcome::Cancelled);
        assert(cancelled.cancel_reason == runtime::TaskCancelReason::ScopeUnavailable);
        assert(!hidden.app.CompleteOwnerTask(task));
    }

    Fixture disabled;
    const auto task = disabled.Begin();
    disabled.Ready(task);
    assert(disabled.app.SetBinding("task_enabled", false));
    assert(disabled.Take(task, runtime::TaskOutcome::Cancelled).cancel_reason ==
           runtime::TaskCancelReason::ScopeUnavailable);
}

void CheckCandidatePreservationAndUiReplacement()
{
    Fixture f;
    const auto identity = f.Begin();
    f.Ready(identity);
    const auto installed = f.app.impl_->installed_ui;
    const auto token = f.Scene().OwnerModalToken();
    const auto candidate = f.app.BeginUiLoad();
    assert(candidate != installed);

    bool preparation_failed = false;
    try {
        runtime::PrepareComponent("Card { Button(");
    } catch (const runtime::LoadFailure &) {
        preparation_failed = true;
    }
    assert(preparation_failed);
    assert(f.app.impl_->installed_ui == installed);
    assert(f.app.ActiveOwnerTask()->identity == identity);
    assert(f.Scene().OwnerModalToken() == token);
    f.app.CancelUiLoad();
    assert(f.app.ActiveOwnerTask()->phase == runtime::TaskPhase::Ready);
    assert(f.Scene().OwnerModalToken() == token);

    const auto replacement = f.app.BeginUiLoad();
    f.app.impl_->CommitScene(replacement, MakeScene(), {});
    assert(f.app.impl_->installed_ui == replacement);
    const auto cancelled = f.Take(identity, runtime::TaskOutcome::Cancelled);
    assert(cancelled.cancel_reason == runtime::TaskCancelReason::UiReplaced);
    assert(!f.app.CompleteOwnerTask(identity));
    const auto next = f.Begin();
    assert(next.owner == identity.owner && next.request.value > identity.request.value);
    f.Ready(next);
    assert(f.app.CompleteOwnerTask(next));
    f.Take(next, runtime::TaskOutcome::Success);
}

void CheckRetireCloseAndFailure()
{
    for (int mode = 0; mode < 3; ++mode) {
        Fixture f;
        const auto identity = f.Begin();
        f.Ready(identity);
        assert(f.app.SetOwnerTaskWorking(identity));
        if (mode == 0) {
            f.app.RetireOwnerTasks();
            f.app.RetireOwnerTasks();
        } else if (mode == 1) {
            f.app.Close();
            f.app.Close();
        } else {
            f.app.impl_->FailFrontend();
        }
        assert(f.app.impl_->owner_tasks_retired);
        assert(!f.app.impl_->owner_task_scope);
        assert(!f.app.ActiveOwnerTask());
        assert(!f.app.TakeOwnerTaskTerminal());
        assert(!f.app.BeginOwnerTask("task"));
        assert(!f.app.SetOwnerTaskWorking(identity));
        assert(!f.app.ResumeOwnerTask(identity));
        assert(!f.app.CompleteOwnerTask(identity));
        assert(!f.app.CancelOwnerTask(identity));
        assert(!f.app.FailOwnerTask(identity,
                                    {runtime::TaskFailureCode::OperationFailed, "late provider"}));
    }

    Fixture completed;
    const auto identity = completed.Begin();
    completed.Ready(identity);
    assert(completed.app.CompleteOwnerTask(identity));
    completed.app.RetireOwnerTasks();
    assert(!completed.app.TakeOwnerTaskTerminal()); // No queued terminal reaches an exited owner.
}

void CheckLocalScopeAndStablePresentationChanges()
{
    Fixture first;
    Fixture second;
    const auto a = first.Begin();
    first.Ready(a);
    assert(!first.Click("outside").activation);
    const auto inside = first.Click("inside");
    assert(inside.activation && inside.activation->action == "inside");
    const auto other = second.Click("outside");
    assert(other.activation && other.activation->action == "outside");
    assert(first.app.ActiveOwnerTask()->identity == a);

    const auto b = second.Begin();
    second.Ready(b);
    assert(a.owner != b.owner && a.request == b.request);
    assert(!first.app.CompleteOwnerTask(b));
    assert(!second.app.CancelOwnerTask(a));

    const auto token = first.Scene().OwnerModalToken();
    first.Scene().HandleInput(contracts::FocusEvent{window, false, keyboard, false}, first.shown);
    first.app.impl_->ReconcileOwnerTask();
    assert(first.app.ActiveOwnerTask()->identity == a);
    assert(first.Scene().OwnerModalToken() == token);
    assert(first.Scene().SetViewport({500, 240}));
    first.Ready(a);
    contracts::ThemeSnapshot theme;
    theme.id = "task-fixture";
    theme.name = "Task Fixture";
    theme.generation = 7;
    assert(first.app.ApplyTheme(theme));
    assert(first.app.ActiveOwnerTask()->identity == a);
    assert(first.Scene().OwnerModalToken() == token);
    assert(second.app.ActiveOwnerTask()->identity == b);
    assert(first.app.CompleteOwnerTask(a));
    first.Take(a, runtime::TaskOutcome::Success);
    assert(second.app.ActiveOwnerTask()->identity == b);
    assert(second.app.CompleteOwnerTask(b));
    second.Take(b, runtime::TaskOutcome::Success);
}

void CheckTerminalBeforeReentry()
{
    Fixture f;
    const auto first = f.Begin();
    f.Ready(first);
    assert(f.app.CompleteOwnerTask(first));
    assert(!f.app.BeginOwnerTask("task"));
    const auto terminal = f.Take(first, runtime::TaskOutcome::Success);

    // Business receives an owning result only after the terminal slot is drained.
    const auto next = f.Begin();
    assert(next.owner == terminal.identity.owner);
    assert(next.request.value > terminal.identity.request.value);
    assert(!f.app.CompleteOwnerTask(terminal.identity));
    assert(!f.app.CancelOwnerTask(terminal.identity));
    f.Ready(next);
    assert(f.app.CancelOwnerTask(next, runtime::TaskCancelReason::User));
    f.Take(next, runtime::TaskOutcome::Cancelled);
}

class CancellationCallbackFailure : public std::runtime_error {
public:
    CancellationCallbackFailure() : std::runtime_error("Fixture cancellation callback failed")
    {
    }
};

class ReentrantGestureReceiver {
public:
    ReentrantGestureReceiver(sdk::ClientApplication &app, bool create_next)
        : app_(app), create_next_(create_next)
    {
        app_.OnGesture(std::bind_front(&ReentrantGestureReceiver::Handle, this));
    }

    ~ReentrantGestureReceiver()
    {
        app_.OnGesture({});
    }

    void Handle(const contracts::GestureEvent &event)
    {
        assert(event.action == "outside-gesture");
        if (event.phase == contracts::GesturePhase::Begin) {
            assert(!gesture && !app_.ActiveOwnerTask());
            gesture = event.id;
            return;
        }

        assert(event.phase == contracts::GesturePhase::Cancel && event.id == gesture);
        ++cancellations;
        const auto active = app_.ActiveOwnerTask();
        assert(active && active->phase == runtime::TaskPhase::Preparing);
        first = active->identity;
        assert(app_.CancelOwnerTask(*first, runtime::TaskCancelReason::User));
        if (create_next_) {
            consumed_first = app_.TakeOwnerTaskTerminal();
            assert(consumed_first && consumed_first->identity == *first &&
                   consumed_first->outcome == runtime::TaskOutcome::Cancelled &&
                   consumed_first->cancel_reason == runtime::TaskCancelReason::User);
            next = app_.BeginOwnerTask("task");
            assert(next && next->owner == first->owner &&
                   next->request.value > first->request.value);
            assert(app_.CancelOwnerTask(*next, runtime::TaskCancelReason::Escape));
        }

        throw CancellationCallbackFailure();
    }

    std::uint64_t gesture{};
    unsigned cancellations{};
    std::optional<runtime::TaskIdentity> first;
    std::optional<runtime::TaskIdentity> next;
    std::optional<runtime::TaskTerminal> consumed_first;

private:
    sdk::ClientApplication &app_;
    const bool create_next_;
};

void StartDeliveredGesture(Fixture &fixture, ReentrantGestureReceiver &receiver)
{
    const runtime::InputSnapshotNode *target = nullptr;
    for (const auto &node : fixture.shown->nodes) {
        if (node.action == "outside-drag") {
            target = &node;
            break;
        }
    }
    assert(target && target->bounds.width > 20 && target->bounds.height > 0);
    const contracts::LogicalPoint point{target->bounds.x + target->bounds.width / 2,
                                        target->bounds.y + target->bounds.height / 2};
    fixture.app.impl_->HandleWindowEvent(
        contracts::PointerButtonEvent{window, point, contracts::PointerButton::Primary,
                                      contracts::ButtonState::Pressed, 0, 1, pointer, 781},
        fixture.shown);
    fixture.app.impl_->HandleWindowEvent(
        contracts::PointerMotionEvent{window, {point.x + 10, point.y}, 2, pointer}, fixture.shown);

    assert(receiver.gesture && receiver.cancellations == 0);
    assert(fixture.Scene().State(target->id).dragging);
    assert(!fixture.app.ActiveOwnerTask());
}

void CheckCallbackAcceptedTerminalSurvivesBeginException()
{
    for (const bool create_next : std::array{false, true}) {
        Fixture f;
        ReentrantGestureReceiver receiver(f.app, create_next);
        StartDeliveredGesture(f, receiver);

        bool threw = false;
        std::optional<runtime::TaskIdentity> returned;
        try {
            returned = f.app.BeginOwnerTask("task");
        } catch (const CancellationCallbackFailure &) {
            threw = true;
        }

        // Opening the scope cancels a real, already-delivered drag. Its callback
        // owns the observed task identity and deliberately accepts the terminal
        // before throwing; outer Begin rollback must preserve that first result.
        assert(threw && !returned && receiver.first && receiver.cancellations == 1);
        assert(!f.app.ActiveOwnerTask() && !f.app.impl_->owner_task_scope);
        assert(!f.Scene().OwnerModalToken());
        assert(f.app.impl_->bridge->terminal.Reason() == runtime::TerminalReason::None);
        assert(!f.app.BeginOwnerTask("task")); // The accepted terminal still occupies its slot.
        if (create_next) {
            assert(receiver.consumed_first && receiver.next);
            const auto terminal = f.Take(*receiver.next, runtime::TaskOutcome::Cancelled);
            assert(terminal.cancel_reason == runtime::TaskCancelReason::Escape);
            assert(terminal.identity != *receiver.first);
        } else {
            assert(!receiver.consumed_first && !receiver.next);
            const auto terminal = f.Take(*receiver.first, runtime::TaskOutcome::Cancelled);
            assert(terminal.cancel_reason == runtime::TaskCancelReason::User);
        }
        assert(!f.app.CompleteOwnerTask(*receiver.first));
        const auto fresh = f.Begin();
        const auto previous = receiver.next.value_or(*receiver.first);
        assert(fresh.owner == previous.owner && fresh.request.value > previous.request.value);
        f.Ready(fresh);
        assert(f.app.CompleteOwnerTask(fresh));
        f.Take(fresh, runtime::TaskOutcome::Success);
    }
}

} // namespace

int main()
{
    CheckBeginEligibility();
    CheckPublicationFailureRollsBack();
    CheckUnresolvedLayoutCanRetry();
    CheckQueuedAndSuccessfulMetadata();
    CheckOldEpochAndForeignUi();
    CheckWorkingRecoveryAndFailure();
    CheckEscapeAndUnavailableScope();
    CheckCandidatePreservationAndUiReplacement();
    CheckRetireCloseAndFailure();
    CheckLocalScopeAndStablePresentationChanges();
    CheckTerminalBeforeReentry();
    CheckCallbackAcceptedTerminalSurvivesBeginException();
}
