#include "client_application_p.hpp"

#include "prism/contracts/display_list_validation.hpp"
#include "prism/runtime/owner_file_panel.hpp"
#include "prism/runtime/owner_task_panel.hpp"
#include "prism/runtime/task_presentation.hpp"
#include "prism/theme/compiler.hpp"
#include "prism/theme/motion_compiler.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace prism;

namespace {
const std::filesystem::path source_root{PRISM_SOURCE_ROOT};
constexpr contracts::WindowId window{1};

// This suite verifies provider/lifecycle policy at zero duration. Nonzero
// task motion has its own fake-clock FramePacket/adoption integration suite.
contracts::ThemeSnapshot InstantTheme(const std::filesystem::path &theme_root, std::string_view id,
                                      std::uint64_t generation = 0,
                                      std::string_view scheme = "dark")
{
    auto snapshot = theme::LoadTheme(theme_root, id, generation, scheme);
    snapshot.motion = theme::LoadMotion(theme_root.parent_path() / "motions", "instant");
    return snapshot;
}

constexpr contracts::InputSource pointer{0, 1, 1};

std::string Read(const std::filesystem::path &path)
{
    std::ifstream input(path);
    assert(input);
    return {std::istreambuf_iterator<char>(input), {}};
}

sdk::ClientConfig Config()
{
    sdk::ClientConfig config;
    config.app_id = "task-presentation-sdk-fixture";
    config.font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    config.width = 640;
    config.height = 420;
    return config;
}

runtime::PreparedComponent Master()
{
    return runtime::PrepareComponent(R"(
Card(material:"window", padding:"@window_padding") {
    VStack(spacing:8) {
        Text("Current document", height:22, font:"@font_body", foreground:"@text")
        Button("Owner action", action:"owner", width:$owner_width, height:32)
        Card(flex:1)
    }
})");
}

contracts::OwnerTaskRequest Confirmation()
{
    contracts::OwnerTaskRequest request;
    request.request_id = 19;
    request.title = "Unsaved changes";
    request.message = "Save this document before closing?";
    request.choices = {{1, "Save", contracts::OwnerTaskChoiceRole::Primary},
                       {2, "Discard", contracts::OwnerTaskChoiceRole::Destructive}};
    assert(contracts::ValidateOwnerTaskRequest(request));
    return request;
}

contracts::OwnerTaskRequest SaveFile()
{
    contracts::OwnerTaskRequest request;
    request.request_id = 20;
    request.kind = contracts::OwnerTaskKind::SaveFile;
    request.title = "Save document";
    request.file = contracts::OwnerFileTaskOptions{"/tmp", "draft.txt", {}, false};
    assert(contracts::ValidateOwnerTaskRequest(request));
    return request;
}

runtime::OwnerFilePanelView FileView()
{
    runtime::OwnerFilePanelView view;
    view.title = "Save document";
    view.directory = "/tmp";
    view.filename = "draft.txt";
    view.status = "Choose a location";
    view.page_caption = "1 / 1";
    view.selected_caption = "/tmp/draft.txt";
    view.rows[0] = {"draft.txt", false, true};
    view.nav_enabled = true;
    view.submit_enabled = true;
    view.show_filename = true;
    return view;
}

// Real shared DSL, SDK text shaping, Scene composition and immutable packets.
// Metadata receipts are deliberately controlled: no Wayland connection, render
// worker or GPU is opened, and this fixture is not native submission evidence.
struct Fixture {
    sdk::ClientApplication app{Config()};

    Fixture()
    {
        assert(app.FrontendReady());
        assert(app.ApplyTheme(InstantTheme(source_root / "resources/themes", "glass", 1)));
        assert(app.ConfigureOwnerTaskPanel(
            runtime::PrepareComponent(Read(source_root / "resources/ui/owner-task-panel.prism"))));
        assert(app.ConfigureOwnerFilePanel(
            runtime::PrepareComponent(Read(source_root / "resources/ui/owner-file-panel.prism"))));

        auto &impl = *app.impl_;
        impl.opened_once = true;
        impl.ui_configure_count = 1;
        impl.ui_metrics = {{640, 420}, {640, 420}, 1};
        impl.binding_values = {{"owner_width", 120.0}};
        Install();
        Submit(Current());
        assert(!impl.render_owner && !impl.worker_generation);
        assert(app.SupportsOwnerConfirmation() && app.SupportsOwnerFileTasks());
        assert(!app.OwnerTaskPresentation());
    }

    runtime::Scene &Scene()
    {
        assert(app.impl_->scene);
        return *app.impl_->scene;
    }

    void Install()
    {
        const auto load = app.BeginUiLoad();
        runtime::LoadDiagnostic diagnostic;
        assert(app.impl_->InstallScene(load, Master(), &diagnostic));
        assert(diagnostic.message.empty());
        app.impl_->PublishFramePacket();
    }

    std::shared_ptr<const runtime::FramePacket> Current()
    {
        app.impl_->PublishFramePacket();
        const auto frame = app.impl_->queued_frame;
        assert(frame && frame->input_snapshot && frame->display_list);
        return frame;
    }

    void Submit(const std::shared_ptr<const runtime::FramePacket> &frame,
                bool metadata_prepared = true,
                runtime::SubmittedKind kind = runtime::SubmittedKind::None)
    {
        contracts::ValidateDisplayList(*frame->display_list);
        runtime::SubmittedFrameEvent event;
        event.ui = frame->ui;
        event.frame_sequence = frame->sequence;
        event.frame = frame;
        event.kind = kind;
        event.scene_revision = frame->scene_revision;
        event.pixels_revision = frame->pixels_revision;
        event.theme_generation = frame->theme_generation;
        event.metadata_prepared = metadata_prepared;
        app.impl_->HandleSubmitted(event);
        assert(!app.impl_->failed);
    }

    runtime::TaskIdentity Begin(bool file = false)
    {
        const auto identity = file ? app.BeginOwnerFileTask(SaveFile(), FileView())
                                   : app.BeginOwnerConfirmation(Confirmation());
        assert(identity && app.ActiveOwnerTask()->identity == identity);
        assert(app.ActiveOwnerTask()->phase == runtime::TaskPhase::Preparing);
        assert(app.impl_->owner_task_scope && Scene().OwnerModalToken());
        assert(Current()->task_presentation);
        return *identity;
    }

    runtime::TaskPresentationState State()
    {
        const auto value = app.OwnerTaskPresentation();
        assert(value);
        return *value;
    }

    runtime::InteractionResult Click(std::string_view action,
                                     const std::shared_ptr<const runtime::InputSnapshot> &input)
    {
        const auto target =
            std::find_if(input->nodes.begin(), input->nodes.end(), [action](const auto &node) {
                return node.visible && node.action == action;
            });
        assert(target != input->nodes.end());
        const contracts::LogicalPoint point{target->bounds.x + target->bounds.width / 2,
                                            target->bounds.y + target->bounds.height / 2};
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
        Scene().HandleInput(down, input);
        return Scene().HandleInput(up, input);
    }

    runtime::TaskTerminal Take(runtime::TaskIdentity identity, runtime::TaskOutcome outcome)
    {
        assert(!app.ActiveOwnerTask() && !app.impl_->owner_task_scope);
        assert(!Scene().OwnerModalToken());
        assert(!app.impl_->owner_confirmation && !app.impl_->owner_file_view);
        assert(app.impl_->owner_task_bindings.empty());
        assert(!Scene().IsVisible(Scene().RegionId(runtime::kOwnerTaskPanelRegion)));
        const auto terminal = app.TakeOwnerTaskTerminal();
        assert(terminal && terminal->identity == identity && terminal->outcome == outcome);
        assert(!app.TakeOwnerTaskTerminal());
        return *terminal;
    }

    void Resize(int width, int height)
    {
        auto &impl = *app.impl_;
        impl.HandleWindowEvent(
            contracts::ConfigureEvent{
                window,
                {{static_cast<double>(width), static_cast<double>(height)},
                 {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)},
                 1},
                impl.ui_configure_count + 1},
            Scene().InputGeometry());
        impl.PublishFramePacket();
    }
};

using Phase = runtime::TaskPresentationPhase;

void CheckStamp(const runtime::TaskPresentationState &state,
                const runtime::TaskPresentationStamp &stamp,
                runtime::TaskPresentationEndpoint endpoint)
{
    assert(state.identity == stamp.identity);
    assert(state.projection == stamp.projection);
    assert(state.binding && *state.binding == stamp.binding);
    assert(state.adopted_sequence == stamp.frame_sequence);
    assert(stamp.endpoint == endpoint);
}

void CheckOpeningRequiresMatchingAdoption()
{
    for (const auto kind : {runtime::SubmittedKind::None, runtime::SubmittedKind::State}) {
        Fixture f;
        const auto identity = f.Begin();
        const auto frame = f.Current();
        const auto opening = f.State();
        assert(opening.identity.task == identity);
        assert(opening.identity.ui == f.app.impl_->installed_ui && opening.identity.cycle);
        assert(opening.phase == Phase::Opening && !opening.adopted_sequence);
        assert(frame->task_presentation->endpoint == runtime::TaskPresentationEndpoint::Open);
        assert(frame->task_presentation->binding.input_scene == frame->input_snapshot->scene);
        assert(frame->task_presentation->binding.input_version == frame->input_snapshot->version);
        assert(!f.Scene().IsInputSnapshotAdopted(*frame->input_snapshot));
        assert(!f.app.CompleteOwnerTask(identity));

        f.Submit(frame, false, kind);
        assert(f.State() == opening);
        assert(f.app.ActiveOwnerTask()->phase == runtime::TaskPhase::Preparing);

        const auto pixels = f.Scene().PixelsRevision();
        f.Submit(frame, true, kind);
        assert(f.Scene().PixelsRevision() == pixels);
        assert(f.Scene().IsInputSnapshotAdopted(*frame->input_snapshot));
        assert(f.State().phase == Phase::Open);
        CheckStamp(f.State(), *frame->task_presentation, runtime::TaskPresentationEndpoint::Open);
        assert(f.app.ActiveOwnerTask()->phase == runtime::TaskPhase::Ready);
        assert(f.app.SetOwnerTaskWorking(identity));
        assert(f.State().phase == Phase::Open);
        assert(f.app.CancelOwnerTask(identity));
        f.Take(identity, runtime::TaskOutcome::Cancelled);
    }
}

enum class BadProof {
    Missing,
    ForeignTask,
    ForeignUi,
    ForeignCycle,
    Projection,
    Endpoint,
    FrameSequence,
    InputScene,
    InputVersion,
    ModalToken,
    ModalEpoch,
    Root,
    Configure,
    Buffer,
    Scale,
    Theme,
    MissingInput
};

void Corrupt(runtime::FramePacket &frame, BadProof proof)
{
    auto &stamp = *frame.task_presentation;
    switch (proof) {
    case BadProof::Missing:
        frame.task_presentation.reset();
        break;
    case BadProof::ForeignTask:
        ++stamp.identity.task.owner.value;
        break;
    case BadProof::ForeignUi:
        ++stamp.identity.ui.owner;
        break;
    case BadProof::ForeignCycle:
        ++stamp.identity.cycle;
        break;
    case BadProof::Projection:
        ++stamp.projection;
        break;
    case BadProof::Endpoint:
        stamp.endpoint = runtime::TaskPresentationEndpoint::Closed;
        break;
    case BadProof::FrameSequence:
        ++stamp.frame_sequence;
        break;
    case BadProof::InputScene:
        ++stamp.binding.input_scene;
        break;
    case BadProof::InputVersion:
        ++stamp.binding.input_version;
        break;
    case BadProof::ModalToken:
        ++stamp.binding.modal_token;
        break;
    case BadProof::ModalEpoch:
        ++stamp.binding.modal_epoch;
        break;
    case BadProof::Root:
        ++stamp.binding.root.generation;
        break;
    case BadProof::Configure:
        ++frame.configure_count;
        break;
    case BadProof::Buffer:
        ++frame.buffer_size.width;
        break;
    case BadProof::Scale:
        frame.scale = 2;
        break;
    case BadProof::Theme:
        ++frame.theme_generation;
        break;
    case BadProof::MissingInput:
        frame.input_snapshot.reset();
        break;
    }
}

void CheckMalformedAndLegacyReceiptsCannotOpen()
{
    constexpr std::array proofs{
        BadProof::Missing,       BadProof::ForeignTask, BadProof::ForeignUi,
        BadProof::ForeignCycle,  BadProof::Projection,  BadProof::Endpoint,
        BadProof::FrameSequence, BadProof::InputScene,  BadProof::InputVersion,
        BadProof::ModalToken,    BadProof::ModalEpoch,  BadProof::Root,
        BadProof::Configure,     BadProof::Buffer,      BadProof::Scale,
        BadProof::Theme,         BadProof::MissingInput};
    for (const auto proof : proofs) {
        Fixture f;
        const auto identity = f.Begin();
        const auto valid = f.Current();
        const auto opening = f.State();
        auto bad = std::make_shared<runtime::FramePacket>(*valid);
        Corrupt(*bad, proof);
        f.Submit(bad);
        assert(f.State() == opening);
        assert(f.app.ActiveOwnerTask()->identity == identity);

        f.Submit(valid);
        assert(f.State().phase == Phase::Open);
        CheckStamp(f.State(), *valid->task_presentation, runtime::TaskPresentationEndpoint::Open);
    }

    Fixture legacy;
    const auto identity = legacy.Begin();
    const auto frame = legacy.Current();
    legacy.Scene().ApplyInputSnapshot(frame->input_snapshot);
    legacy.app.impl_->AdoptOwnerTaskInput(frame->input_snapshot, frame->ui);
    assert(legacy.app.ActiveOwnerTask()->phase == runtime::TaskPhase::Ready);
    assert(legacy.State().phase == Phase::Opening && !legacy.State().adopted_sequence);
    legacy.Submit(frame);
    assert(legacy.State().phase == Phase::Open);
    assert(legacy.app.CancelOwnerTask(identity));
    legacy.Take(identity, runtime::TaskOutcome::Cancelled);
}

void CheckFileRefreshKeepsCycleAndRejectsOldProof()
{
    Fixture f;
    const auto identity = f.Begin(true);
    auto view = FileView();
    const auto first = f.Current();
    const auto opening = f.State();

    // A directory result can replace the first projection before its frame is
    // adopted. Neither the retired row descriptor nor its receipt can open it.
    view.rows[0] = {"other.txt", false, true};
    view.page_caption = "2 / 2";
    assert(f.app.UpdateOwnerFileTask(identity, view));
    const auto second = f.Current();
    assert(f.State().identity == opening.identity);
    assert(f.State().projection > opening.projection);
    assert(f.State().phase == Phase::Opening && !f.State().adopted_sequence);
    assert(!f.Click(runtime::kOwnerFileRowActions[0], first->input_snapshot).activation);
    f.Submit(first);
    assert(f.State().phase == Phase::Opening && !f.State().adopted_sequence);
    assert(f.app.ActiveOwnerTask()->phase == runtime::TaskPhase::Preparing);
    f.Submit(second);
    assert(f.State().phase == Phase::Open);
    CheckStamp(f.State(), *second->task_presentation, runtime::TaskPresentationEndpoint::Open);

    const auto adopted = f.State();
    assert(f.app.SetOwnerTaskWorking(identity));
    view.loading = true;
    view.nav_enabled = false;
    view.rows = {};
    view.status = "Loading folder";
    assert(f.app.UpdateOwnerFileTask(identity, view));
    const auto third = f.Current();
    assert(f.State().identity == adopted.identity);
    assert(f.State().phase == Phase::Open && f.State().projection > adopted.projection);
    assert(!f.State().adopted_sequence);
    assert(f.app.ActiveOwnerTask()->phase == runtime::TaskPhase::Preparing);
    assert(!f.Click(runtime::kOwnerFileRowActions[0], second->input_snapshot).activation);
    f.Submit(second);
    assert(!f.State().adopted_sequence);
    f.Submit(third, true, runtime::SubmittedKind::State);
    CheckStamp(f.State(), *third->task_presentation, runtime::TaskPresentationEndpoint::Open);
    assert(f.State().phase == Phase::Open);
    assert(f.app.CancelOwnerTask(identity));
    f.Take(identity, runtime::TaskOutcome::Cancelled);
}

void CheckTerminalsRetireInputBeforeClosedAdoption()
{
    for (const auto outcome : {runtime::TaskOutcome::Success, runtime::TaskOutcome::Cancelled,
                               runtime::TaskOutcome::Failed}) {
        Fixture f;
        const auto identity = f.Begin();
        const auto shown = f.Current();
        f.Submit(shown);
        const auto open = f.State();

        if (outcome == runtime::TaskOutcome::Success) {
            assert(f.app.CompleteOwnerTask(identity));
        } else if (outcome == runtime::TaskOutcome::Cancelled) {
            assert(f.app.CancelOwnerTask(identity));
        } else {
            assert(f.app.FailOwnerTask(
                identity, {runtime::TaskFailureCode::OperationFailed, "Operation failed"}));
        }
        assert(f.State().identity == open.identity && f.State().phase == Phase::Closing);
        assert(f.Scene().OwnerModalEpoch() > shown->input_snapshot->owner_modal_epoch);
        assert(!f.Click(runtime::kOwnerTaskChoiceActions[0], shown->input_snapshot).activation);
        const auto hidden = f.Current();
        assert(hidden->task_presentation &&
               hidden->task_presentation->endpoint == runtime::TaskPresentationEndpoint::Closed);
        assert(!hidden->task_presentation->binding.modal_token);
        assert(!hidden->task_presentation->binding.root);
        f.Take(identity, outcome); // Closing cannot hold the business terminal.

        f.Submit(shown);
        assert(f.State().phase == Phase::Closing);
        f.Submit(hidden, false);
        assert(f.State().phase == Phase::Closing);
        f.Submit(hidden, true, runtime::SubmittedKind::State);
        assert(f.State().phase == Phase::Closed);
        CheckStamp(f.State(), *hidden->task_presentation,
                   runtime::TaskPresentationEndpoint::Closed);
        assert(!f.app.TakeOwnerTaskTerminal());
    }
}

void CheckQuickCancelAndReentryReplaceClosing()
{
    Fixture quick;
    const auto identity = quick.Begin();
    const auto never_adopted = quick.Current();
    assert(quick.app.CancelOwnerTask(identity));
    assert(quick.State().phase == Phase::Closed && !quick.State().adopted_sequence);
    quick.Take(identity, runtime::TaskOutcome::Cancelled);
    quick.Submit(never_adopted);
    assert(quick.State().phase == Phase::Closed);

    Fixture f;
    const auto first = f.Begin();
    f.Submit(f.Current());
    assert(f.app.CompleteOwnerTask(first));
    const auto closing = f.Current();
    const auto previous = f.State();
    f.Take(first, runtime::TaskOutcome::Success);
    const auto second = f.Begin(true); // Notepad Save confirmation -> SaveFile chain.
    const auto replacement = f.Current();
    assert(second.owner == first.owner && second.request.value > first.request.value);
    assert(f.State().identity.task == second && f.State().identity.cycle > previous.identity.cycle);
    assert(f.State().phase == Phase::Opening);
    f.Submit(closing);
    assert(f.State().identity.task == second && f.State().phase == Phase::Opening);
    assert(!f.app.CompleteOwnerTask(first) && !f.app.CancelOwnerTask(first));
    assert(!f.app.RefreshOwnerTask(first));
    f.Submit(replacement);
    assert(f.State().phase == Phase::Open);
    CheckStamp(f.State(), *replacement->task_presentation, runtime::TaskPresentationEndpoint::Open);
    assert(f.app.CancelOwnerTask(second));
    f.Take(second, runtime::TaskOutcome::Cancelled);
}

void CheckResizeAndThemeReprojectWithoutReopening()
{
    Fixture f;
    const auto identity = f.Begin(true);
    const auto old = f.Current();
    f.Submit(old);
    const auto first = f.State();
    f.Resize(500, 360);
    const auto resized = f.Current();
    assert(f.app.ActiveOwnerTask()->identity == identity);
    assert(f.State().identity == first.identity && f.State().phase == Phase::Open);
    assert(f.State().projection > first.projection && !f.State().adopted_sequence);
    f.Submit(old);
    assert(!f.State().adopted_sequence);
    f.Submit(resized);
    CheckStamp(f.State(), *resized->task_presentation, runtime::TaskPresentationEndpoint::Open);

    const auto before_theme = f.State();
    assert(f.app.ApplyTheme(InstantTheme(source_root / "resources/themes", "square", 2, "light")));
    const auto themed = f.Current();
    assert(f.app.ActiveOwnerTask()->identity == identity);
    assert(f.State().identity == first.identity && f.State().phase == Phase::Open);
    assert(f.State().projection > before_theme.projection && !f.State().adopted_sequence);
    f.Submit(resized);
    assert(!f.State().adopted_sequence);
    f.Submit(themed);
    CheckStamp(f.State(), *themed->task_presentation, runtime::TaskPresentationEndpoint::Open);
    assert(f.app.CancelOwnerTask(identity));
    f.Take(identity, runtime::TaskOutcome::Cancelled);
}

void CheckCandidateLoadReplacementAndOwnerRetirement()
{
    Fixture f;
    const auto identity = f.Begin();
    const auto previous = f.Current();
    f.Submit(previous);
    const auto open = f.State();
    f.app.BeginUiLoad();
    f.app.CancelUiLoad();
    assert(f.State() == open && f.app.ActiveOwnerTask()->identity == identity);
    f.Install();
    assert(f.State().phase == Phase::Closed);
    assert(f.State().interruption == runtime::TaskPresentationInterruptReason::UiReplaced);
    f.Take(identity, runtime::TaskOutcome::Cancelled);
    f.Submit(previous);
    assert(f.State().phase == Phase::Closed);
    f.Submit(f.Current());
    const auto next = f.Begin();
    assert(f.State().identity.ui != open.identity.ui && f.State().identity.task == next);

    for (const bool fail : {false, true}) {
        Fixture retired;
        const auto request = retired.Begin(true);
        retired.Submit(retired.Current());
        if (fail) {
            retired.app.impl_->FailFrontend();
        } else {
            retired.app.RetireOwnerTasks();
        }
        assert(retired.State().phase == Phase::Closed);
        assert(!retired.app.ActiveOwnerTask() && !retired.app.TakeOwnerTaskTerminal());
        assert(!retired.app.BeginOwnerConfirmation(Confirmation()));
        assert(!retired.app.CompleteOwnerTask(request));
    }

    Fixture other;
    const auto foreign = other.Begin();
    other.Submit(other.Current());
    assert(f.State().identity.task.owner != other.State().identity.task.owner);
    assert(!f.app.CancelOwnerTask(foreign));
    assert(other.State().phase == Phase::Open && f.State().phase == Phase::Opening);
}

void CheckUndersizedFileTaskFailsOnce()
{
    Fixture f;
    const auto identity = f.Begin(true);
    f.Submit(f.Current());
    f.Resize(319, 420);
    assert(!f.app.ActiveOwnerTask() && !f.app.impl_->owner_task_scope);
    const auto terminal = f.Take(identity, runtime::TaskOutcome::Failed);
    assert(terminal.failure &&
           terminal.failure->code == runtime::TaskFailureCode::PreparationFailed);
    assert(f.State().phase == Phase::Closed || f.State().phase == Phase::Closing);
    f.Submit(f.Current());
    assert(f.State().phase == Phase::Closed);
    f.Resize(640, 420);
    assert(!f.app.ActiveOwnerTask() && !f.app.TakeOwnerTaskTerminal());
}

void CheckClosingGeometryAndThemeInterruptImmediately()
{
    for (const bool theme_change : {false, true}) {
        Fixture f;
        const auto identity = f.Begin(true);
        f.Submit(f.Current());
        assert(f.app.CancelOwnerTask(identity));
        const auto closing = f.Current();
        assert(f.State().phase == Phase::Closing);
        f.Take(identity, runtime::TaskOutcome::Cancelled);

        if (theme_change) {
            assert(f.app.ApplyTheme(
                InstantTheme(source_root / "resources/themes", "square", 2, "light")));
        } else {
            f.Resize(500, 360);
        }
        const auto closed = f.State();
        assert(closed.phase == Phase::Closed && !closed.binding && !closed.adopted_sequence);
        assert(closed.interruption ==
               (theme_change ? runtime::TaskPresentationInterruptReason::ThemeChanged
                             : runtime::TaskPresentationInterruptReason::GeometryChanged));
        assert(!f.Current()->task_presentation);
        f.Submit(closing);
        assert(f.State() == closed && !f.app.TakeOwnerTaskTerminal());
        f.Submit(f.Current());
        assert(f.State() == closed);
    }
}

void CheckStaleLiveGeometryCannotCompleteEndpoints()
{
    for (const bool closing : {false, true}) {
        for (const bool resolve_before_receipt : {false, true}) {
            Fixture f;
            const auto identity = f.Begin();
            if (closing) {
                f.Submit(f.Current());
                assert(f.app.CancelOwnerTask(identity));
                f.Take(identity, runtime::TaskOutcome::Cancelled);
            }
            const auto old = f.Current();
            const auto before = f.State();
            assert(before.phase == (closing ? Phase::Closing : Phase::Opening));

            // Change real application geometry without publishing a replacement
            // packet. The old stamp is still the core's last published binding.
            assert(f.app.SetBinding("owner_width", 224.0));
            assert(runtime::Has(f.Scene().PendingDirty(), runtime::Dirty::Layout));
            if (resolve_before_receipt) {
                f.Scene().ResolveLayout();
                assert(!runtime::Has(f.Scene().PendingDirty(), runtime::Dirty::Layout));
                assert(f.Scene().CaptureInputSnapshot()->version > old->input_snapshot->version);
            }
            f.Submit(old);
            assert(f.State() == before);

            const auto current = f.Current();
            assert(current->input_snapshot->version > old->input_snapshot->version);
            assert(current->task_presentation->projection > before.projection);
            f.Submit(current);
            assert(f.State().phase == (closing ? Phase::Closed : Phase::Open));
            CheckStamp(f.State(), *current->task_presentation,
                       closing ? runtime::TaskPresentationEndpoint::Closed
                               : runtime::TaskPresentationEndpoint::Open);
            if (!closing) {
                assert(f.app.CancelOwnerTask(identity));
                f.Take(identity, runtime::TaskOutcome::Cancelled);
            }
        }
    }
}

class ActionReceiver {
public:
    explicit ActionReceiver(sdk::ClientApplication &app) : app_(app)
    {
        app_.OnAction(std::bind_front(&ActionReceiver::Handle, this));
    }

    ~ActionReceiver()
    {
        app_.OnAction({});
    }

    void Handle(std::string_view)
    {
        ++calls;
    }

    std::uint64_t calls{};

private:
    sdk::ClientApplication &app_;
};

void QueueLateTaskInput(Fixture &fixture, const std::shared_ptr<const runtime::FramePacket> &frame)
{
    const auto target =
        std::find_if(frame->input_snapshot->nodes.begin(), frame->input_snapshot->nodes.end(),
                     [](const auto &node) {
                         return node.visible && node.action == runtime::kOwnerTaskChoiceActions[0];
                     });
    assert(target != frame->input_snapshot->nodes.end());
    const contracts::LogicalPoint point{target->bounds.x + target->bounds.width / 2,
                                        target->bounds.y + target->bounds.height / 2};
    for (const auto state : {contracts::ButtonState::Pressed, contracts::ButtonState::Released}) {
        const contracts::PointerButtonEvent pointer_event{
            window, point, contracts::PointerButton::Primary, state, 0, 1, pointer, 781};
        runtime::RenderEvent event(runtime::SequencedWindowEvent{
            pointer_event, state == contracts::ButtonState::Pressed ? 1u : 2u, frame->ui,
            frame->input_snapshot});
        assert(fixture.app.impl_->bridge->render_events.TryPush(std::move(event)) ==
               runtime::QueuePushResult::Accepted);
    }
}

void CheckCloseAndAcceptedCloseRetireBeforeLateEvents()
{
    for (const bool accept : {false, true}) {
        Fixture f;
        ActionReceiver receiver(f.app);
        const auto identity = f.Begin();
        const auto frame = f.Current();
        f.Submit(frame);
        const auto cycle = f.State().identity;

        if (accept) {
            assert(f.app.AcceptClose());
            assert(f.app.AcceptClose());
            QueueLateTaskInput(f, frame);
            f.app.impl_->ProcessRenderEvents(true);
            assert(f.app.impl_->close_accept_queued);
        } else {
            QueueLateTaskInput(f, frame);
            f.app.Close();
            f.app.Close();
            f.app.impl_->ProcessRenderEvents(true);
            assert(f.app.impl_->closed);
        }
        assert(receiver.calls == 0);
        assert(f.State().identity == cycle && f.State().phase == Phase::Closed);
        assert(f.State().interruption == runtime::TaskPresentationInterruptReason::OwnerRetired);
        assert(!f.app.impl_->owner_task_scope);
        assert(!f.app.ActiveOwnerTask() && !f.app.TakeOwnerTaskTerminal());
        assert(!f.app.CompleteOwnerTask(identity) && !f.app.CancelOwnerTask(identity));
        assert(!f.app.BeginOwnerConfirmation(Confirmation()));
        assert(!f.app.SupportsOwnerConfirmation() && !f.app.SupportsOwnerFileTasks());
    }
}
} // namespace

int main()
{
    CheckOpeningRequiresMatchingAdoption();
    CheckMalformedAndLegacyReceiptsCannotOpen();
    CheckFileRefreshKeepsCycleAndRejectsOldProof();
    CheckTerminalsRetireInputBeforeClosedAdoption();
    CheckQuickCancelAndReentryReplaceClosing();
    CheckResizeAndThemeReprojectWithoutReopening();
    CheckCandidateLoadReplacementAndOwnerRetirement();
    CheckUndersizedFileTaskFailsOnce();
    CheckClosingGeometryAndThemeInterruptImmediately();
    CheckStaleLiveGeometryCannotCompleteEndpoints();
    CheckCloseAndAcceptedCloseRetireBeforeLateEvents();
    std::cout << "client_owner_task_presentation_test: passed (controlled non-native metadata)\n";
}
