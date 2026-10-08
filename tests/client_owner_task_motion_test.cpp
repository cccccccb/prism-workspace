#include "client_application_p.hpp"

#include "prism/contracts/display_list_validation.hpp"
#include "prism/runtime/owner_file_panel.hpp"
#include "prism/runtime/owner_task_panel.hpp"
#include "prism/runtime/task_motion.hpp"
#include "prism/theme/compiler.hpp"
#include "prism/theme/motion_compiler.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace prism;

namespace {
const std::filesystem::path source_root{PRISM_SOURCE_ROOT};
constexpr contracts::WindowId window{1};
constexpr contracts::InputSource pointer{0, 1, 1};
constexpr std::uint64_t millisecond = 1'000'000;
constexpr std::string_view font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
using Phase = runtime::TaskPresentationPhase;
using Endpoint = runtime::TaskPresentationEndpoint;
using Kind = runtime::TaskPresentationSampleKind;
using Frame = std::shared_ptr<const runtime::FramePacket>;

class ManualClock final : public animation::AnimationClock {
public:
    std::uint64_t NowNs() const noexcept override
    {
        return now;
    }

    std::uint64_t now{};
};

enum class Profile { Timed, Instant, Legacy };

contracts::ThemeSnapshot Theme(std::string_view material = "square", std::uint64_t generation = 1,
                               Profile profile = Profile::Timed)
{
    auto snapshot =
        theme::LoadTheme(source_root / "resources/themes", material, generation, "light");
    if (profile == Profile::Instant) {
        snapshot.motion = theme::LoadMotion(source_root / "resources/motions", "instant");
    } else if (profile == Profile::Legacy) {
        snapshot.schema_version = 2;
        snapshot.motion = {};
    } else {
        snapshot.motion = {"task-motion-fixture",
                           {{"task.open", 100, contracts::MotionEasing::Linear},
                            {"task.close", 100, contracts::MotionEasing::Linear},
                            {"control.feedback", 0, contracts::MotionEasing::Linear}}};
    }
    contracts::ValidateTheme(snapshot);
    return snapshot;
}

std::string Read(const std::filesystem::path &path)
{
    std::ifstream input(path);
    assert(input);
    return {std::istreambuf_iterator<char>(input), {}};
}

runtime::PreparedComponent Provider(const std::filesystem::path &path, Profile profile)
{
    auto source = Read(path);
    if (profile == Profile::Legacy) {
        // Empty legacy MotionSet cannot resolve named Scene transitions. Keep
        // the shared provider's geometry/actions and use its supported literal
        // zero-duration form only for this dedicated legacy fixture.
        constexpr std::string_view named = R"(motion: "control.feedback")";
        constexpr std::string_view literal = R"(durationMs: 0, easing: "linear")";
        std::size_t position = 0;
        std::size_t replaced = 0;
        while ((position = source.find(named, position)) != std::string::npos) {
            source.replace(position, named.size(), literal);
            position += literal.size();
            ++replaced;
        }
        assert(replaced);
    }
    return runtime::PrepareComponent(source);
}

sdk::ClientConfig Config()
{
    sdk::ClientConfig config;
    config.app_id = "task-motion-sdk-fixture";
    config.font_path = font_path;
    config.width = 640;
    config.height = 420;
    return config;
}

runtime::PreparedComponent Master()
{
    return runtime::PrepareComponent(R"(
Card(material:"window", padding:"@window_padding") {
    VStack(spacing:8) {
        Text($document, height:22, font:"@font_body", foreground:"@text")
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

void Near(double actual, double expected)
{
    assert(std::abs(actual - expected) <= 0.0000001);
}

// Real shared DSL, Scene composition, font shaping and FramePackets. The Scene
// borrows a fake monotonic clock; no Wayland connection or render worker opens.
// Pixel/State receipts and worker opportunity identities are controlled values.
// They verify SDK policy, and are not native submission or GPU evidence.
struct Fixture {
    ManualClock clock;
    sdk::ClientApplication app{Config()};
    std::vector<std::string> actions;
    std::uint64_t next_submission{};
    std::uint64_t next_opportunity{};

    explicit Fixture(std::string_view material = "square", Profile profile = Profile::Timed)
    {
        assert(app.FrontendReady() && app.ApplyTheme(Theme(material, 1, profile)));
        assert(app.ConfigureOwnerTaskPanel(
            Provider(source_root / "resources/ui/owner-task-panel.prism", profile)));
        assert(app.ConfigureOwnerFilePanel(
            Provider(source_root / "resources/ui/owner-file-panel.prism", profile)));
        auto &impl = *app.impl_;
        impl.binding_values = {{"owner_width", 120.0},
                               {"document", std::string("Current document")}};
        impl.scene = std::make_unique<runtime::Scene>(
            impl.ComposeOwnerPanels(runtime::LinkComponent(Master())),
            std::bind_front(&sdk::ClientApplication::Impl::ShapeText, &impl), impl.shaper.FontId(),
            impl.theme);
        auto bindings = impl.OwnerPanelDefaults();
        for (const auto &[name, value] : impl.binding_values) {
            bindings.insert_or_assign(name, value);
        }
        assert(Scene().SetViewport({640, 420}) && Scene().PrepareDetached(bindings));
        Scene().EnableAnimations(&clock);
        impl.opened_once = true;
        impl.ui_configure_count = 1;
        impl.ui_metrics = {{640, 420}, {640, 420}, 1};
        impl.installed_ui = app.BeginUiLoad();
        impl.ui_presentation.Install(impl.installed_ui);
        assert(!impl.render_owner && !impl.worker_generation);
        Submit(Current());
        // A nonzero association enables only the UI owner's opportunity gate;
        // it is removed before teardown and never opens a worker lifecycle.
        impl.worker_generation = runtime::RenderWorkerGeneration{71};
        app.OnAction(std::bind_front(&Fixture::RecordAction, this));
        Drain();
    }

    ~Fixture()
    {
        app.OnAction({});
        app.impl_->worker_generation.reset();
    }

    runtime::Scene &Scene()
    {
        assert(app.impl_->scene);
        return *app.impl_->scene;
    }

    runtime::TaskPresentationState State()
    {
        const auto state = app.OwnerTaskPresentation();
        assert(state);
        return *state;
    }

    Frame Current()
    {
        app.impl_->PublishFramePacket();
        const auto frame = app.impl_->queued_frame;
        assert(frame && frame->display_list && frame->input_snapshot);
        return frame;
    }

    void Submit(const Frame &frame, bool metadata_prepared = true,
                runtime::SubmittedKind kind = runtime::SubmittedKind::Pixels)
    {
        contracts::ValidateDisplayList(*frame->display_list);
        runtime::SubmittedFrameEvent event;
        event.ui = frame->ui;
        event.frame_sequence = frame->sequence;
        event.frame = frame;
        event.kind = metadata_prepared ? kind : runtime::SubmittedKind::None;
        if (event.kind == runtime::SubmittedKind::Pixels) {
            event.submission = {++next_submission};
        }
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
        assert(app.impl_->owner_task_scope && Scene().OwnerModalToken());
        assert(Current()->task_presentation);
        return *identity;
    }

    Frame Tick(std::uint64_t milliseconds)
    {
        clock.now = milliseconds * millisecond;
        if (app.impl_->AdvanceAnimations(clock.now)) {
            app.impl_->InvalidateQueuedFrame();
        }
        return Current();
    }

    runtime::AnswerFrameOpportunityCommand Opportunity(std::uint64_t milliseconds)
    {
        Drain();
        clock.now = milliseconds * millisecond;
        const auto id = ++next_opportunity;
        auto &impl = *app.impl_;
        impl.HandleFrameOpportunity(
            {impl.installed_ui, *impl.worker_generation, impl.ui_configure_count, id});
        std::optional<runtime::AnswerFrameOpportunityCommand> answer;
        while (auto command = impl.bridge->render_commands.TryPop()) {
            if (const auto *value =
                    std::get_if<runtime::AnswerFrameOpportunityCommand>(&*command)) {
                assert(!answer && value->id == id);
                answer = *value;
            }
        }
        assert(answer && answer->ui == impl.installed_ui &&
               answer->worker == *impl.worker_generation &&
               answer->configure_count == impl.ui_configure_count);
        return *answer;
    }

    void Drain()
    {
        while (app.impl_->bridge->render_commands.TryPop()) {
        }
    }

    void RecordAction(std::string_view action)
    {
        actions.emplace_back(action);
    }

    void Click(std::string_view action, const Frame &frame)
    {
        const auto &input = frame->input_snapshot;
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
        app.impl_->HandleWindowEvent(down, input);
        app.impl_->HandleWindowEvent(up, input);
    }

    void Take(runtime::TaskIdentity identity, runtime::TaskOutcome outcome)
    {
        const auto terminal = app.TakeOwnerTaskTerminal();
        assert(terminal && terminal->identity == identity && terminal->outcome == outcome);
        assert(!app.ActiveOwnerTask() && !app.impl_->owner_task_scope &&
               !Scene().OwnerModalToken());
        assert(!app.TakeOwnerTaskTerminal());
    }

    void Resize()
    {
        auto &impl = *app.impl_;
        impl.HandleWindowEvent(contracts::ConfigureEvent{window,
                                                         {{500, 360}, {500, 360}, 1},
                                                         impl.ui_configure_count + 1},
                               Scene().InputGeometry());
        impl.PublishFramePacket();
    }

    std::vector<std::uint8_t> Raster(const contracts::DisplayList &list)
    {
        std::vector<std::uint8_t> pixels(640 * 420 * 4);
        assert(app.impl_->commands.Render(list, pixels.data(), 640, 420, 640 * 4));
        return pixels;
    }
};

void Sample(const Frame &frame, double reveal, Endpoint endpoint, Kind kind)
{
    assert(frame->task_motion && frame->task_presentation);
    const auto &motion = *frame->task_motion;
    assert(motion.identity == frame->task_presentation->identity);
    assert(motion.generation && motion.revision);
    Near(motion.sample.reveal, reveal);
    assert(motion.sample.endpoint == endpoint && motion.sample.kind == kind);
    assert(frame->task_presentation->endpoint == endpoint &&
           frame->task_presentation->sample_kind == kind);
}

void InstantEndpoint(const Frame &frame, Endpoint endpoint)
{
    assert(frame->task_presentation);
    assert(frame->task_presentation->endpoint == endpoint &&
           frame->task_presentation->sample_kind == Kind::Terminal);
    if (frame->task_motion) {
        Sample(frame, endpoint == Endpoint::Open ? 1 : 0, endpoint, Kind::Terminal);
    }
}

void CheckTaskOnlyMotionUsesUnifiedOpportunity()
{
    Fixture f;
    f.Begin();
    const auto first = f.Current();
    Sample(first, 0, Endpoint::Open, Kind::Intermediate);
    assert(!f.Scene().HasActiveAnimations());
    assert(f.app.impl_->HasActiveAnimations() && f.app.impl_->animation_worker_active);
    f.Submit(first);
    const auto answer = f.Opportunity(50);
    assert(answer.frame && answer.frame == f.app.impl_->queued_frame);
    assert(answer.frame->sequence > first->sequence);
    assert(answer.frame->scene_revision == first->scene_revision &&
           answer.frame->pixels_revision == first->pixels_revision);
    assert(answer.frame->task_motion->revision > first->task_motion->revision);
    Sample(answer.frame, 0.5, Endpoint::Open, Kind::Intermediate);
    assert(!f.Scene().HasActiveAnimations() && f.app.impl_->HasActiveAnimations());
    f.Submit(answer.frame);
    assert(f.app.impl_->owner_task_motion->adopted == answer.frame->task_motion);
}

void CheckIdleOpportunityKeepsHardDeadline()
{
    Fixture f;
    f.Begin();
    f.Submit(f.Current());
    const auto unchanged = f.Opportunity(0);
    assert(!unchanged.frame);
    assert(f.app.impl_->animation_worker_active && f.app.impl_->HasActiveAnimations());
    assert(f.app.impl_->animation_deadline_ns == 100 * millisecond);
    assert(f.app.impl_->AnimationTimeoutMs(-1) == 100);
    f.clock.now = 100 * millisecond;
    f.app.impl_->AdvanceAnimationDeadline();
    const auto terminal = f.Current();
    Sample(terminal, 1, Endpoint::Open, Kind::Terminal);
    assert(f.State().phase == Phase::Opening);
    f.Submit(terminal);
    assert(f.State().phase == Phase::Open);
    assert(!f.app.impl_->HasActiveAnimations() && !f.app.impl_->animation_worker_active);
}

void CheckOpeningBusinessReadyAndMouseGateAreSeparate()
{
    Fixture f;
    const auto identity = f.Begin();
    const auto first = f.Current();
    assert(f.app.ActiveOwnerTask()->phase == runtime::TaskPhase::Preparing);
    f.Submit(first);
    assert(f.app.ActiveOwnerTask()->phase == runtime::TaskPhase::Ready);
    assert(f.State().phase == Phase::Opening && f.State().adopted_sequence == first->sequence);
    assert(f.app.impl_->owner_task_paint == first->task_paint_candidate);
    f.Click(runtime::kOwnerTaskChoiceActions[0], first);
    assert(f.actions.empty() && f.app.ActiveOwnerTask()->identity == identity);

    const auto terminal = f.Tick(100);
    Sample(terminal, 1, Endpoint::Open, Kind::Terminal);
    assert(f.State().phase == Phase::Opening);
    f.Click(runtime::kOwnerTaskChoiceActions[0], first);
    assert(f.actions.empty());
    f.Submit(terminal, false);
    assert(f.State().phase == Phase::Opening);
    f.Submit(terminal);
    assert(f.State().phase == Phase::Open);
    assert(f.app.impl_->owner_task_motion->adopted == terminal->task_motion);
    f.Click(runtime::kOwnerTaskChoiceActions[0], terminal);
    assert(f.actions == std::vector<std::string>{std::string(runtime::kOwnerTaskChoiceActions[0])});
    assert(f.app.ActiveOwnerTask()->identity == identity);
}

void CheckIntermediateAdoptionRetainsUnmodulatedPaint()
{
    Fixture f;
    f.Begin();
    const auto initial = f.Current();
    assert(initial->task_paint_candidate);
    const auto quarter = f.Tick(25);
    Sample(quarter, 0.25, Endpoint::Open, Kind::Intermediate);
    assert(quarter->task_paint_candidate);
    assert(quarter->task_paint_candidate->commands == initial->task_paint_candidate->commands);
    assert(quarter->display_list->commands != initial->display_list->commands);
    f.Submit(quarter);
    assert(f.State().phase == Phase::Opening);
    assert(f.app.impl_->owner_task_motion->adopted == quarter->task_motion);
    assert(f.app.impl_->owner_task_paint == quarter->task_paint_candidate);
    const auto unseen = f.Tick(75);
    Sample(unseen, 0.75, Endpoint::Open, Kind::Intermediate);
    assert(f.app.impl_->owner_task_motion->adopted == quarter->task_motion);
    assert(f.app.impl_->owner_task_paint == quarter->task_paint_candidate);
}

void CheckDelayedEarlierIntermediateCanStillBeAdopted()
{
    Fixture f;
    f.Begin();
    const auto earlier = f.Tick(25);
    const auto newer = f.Tick(75);
    assert(newer->task_motion->revision > earlier->task_motion->revision);
    f.Submit(earlier);
    assert(f.State().phase == Phase::Opening && f.State().adopted_sequence == earlier->sequence);
    assert(f.app.impl_->owner_task_motion->adopted == earlier->task_motion);
    assert(f.app.impl_->owner_task_paint == earlier->task_paint_candidate);
    f.Submit(newer);
    assert(f.app.impl_->owner_task_motion->adopted == newer->task_motion);
    assert(f.app.impl_->owner_task_paint == newer->task_paint_candidate);
}

void CheckMidOpeningCloseStartsFromAdoptedReveal()
{
    Fixture f;
    const auto identity = f.Begin();
    const auto seen = f.Tick(25);
    f.Submit(seen);
    const auto paint = f.app.impl_->owner_task_paint;
    const auto unseen = f.Tick(75);
    Sample(unseen, 0.75, Endpoint::Open, Kind::Intermediate);
    assert(f.app.CancelOwnerTask(identity));
    f.Take(identity, runtime::TaskOutcome::Cancelled);
    const auto closing = f.Current();
    Sample(closing, 0.25, Endpoint::Closed, Kind::Intermediate);
    assert(f.State().phase == Phase::Closing && f.app.impl_->ClosingOwnerTaskPaint() == paint);
    f.Submit(unseen, true, runtime::SubmittedKind::State);
    assert(f.app.impl_->owner_task_paint == paint);
    f.Submit(closing);
    assert(f.State().phase == Phase::Closing && f.app.impl_->owner_task_paint == paint);
    assert(f.app.impl_->owner_task_motion->adopted == closing->task_motion);
    const auto half = f.Tick(125);
    Sample(half, 0.125, Endpoint::Closed, Kind::Intermediate);
    f.Submit(half);
    assert(f.State().phase == Phase::Closing && f.app.impl_->owner_task_paint == paint);
}

void CheckClosingPaintIsReadonlyAndBodyStaysFresh()
{
    Fixture f;
    const auto identity = f.Begin();
    const auto open = f.Tick(100);
    f.Submit(open);
    const auto paint = f.app.impl_->owner_task_paint;
    const auto saved_commands = paint->commands;
    assert(f.app.CompleteOwnerTask(identity));
    f.Take(identity, runtime::TaskOutcome::Success);
    const auto closing = f.Current();
    Sample(closing, 1, Endpoint::Closed, Kind::Intermediate);
    assert(!closing->task_paint_candidate && !f.Scene().OwnerModalToken());
    assert(closing->input_snapshot->owner_modal_epoch == f.Scene().OwnerModalEpoch());
    assert(!f.Scene().IsVisible(f.Scene().RegionId(runtime::kOwnerTaskPanelRegion)));
    f.Click(runtime::kOwnerTaskChoiceActions[0], open);
    assert(f.actions.empty());
    f.Submit(closing);
    const auto middle = f.Tick(150);
    Sample(middle, 0.5, Endpoint::Closed, Kind::Intermediate);
    f.Submit(middle);
    assert(f.app.impl_->owner_task_paint == paint && paint->commands == saved_commands);
    assert(f.app.impl_->owner_task_motion->adopted == middle->task_motion);
    f.Click("owner", middle);
    assert(f.actions == std::vector<std::string>{"owner"});
    const auto before = f.Raster(*middle->display_list);

    assert(f.app.SetBinding("document", std::string("Document changed while task closes")));
    assert(f.app.SetBinding("owner_width", 224.0));
    const auto fresh = f.Current();
    Sample(fresh, 0.5, Endpoint::Closed, Kind::Intermediate);
    assert(fresh->display_list->commands != middle->display_list->commands);
    assert(f.Raster(*fresh->display_list) != before);
    assert(f.app.impl_->owner_task_paint == paint && paint->commands == saved_commands);
    f.Submit(fresh);
    assert(f.State().phase == Phase::Closing && f.app.impl_->owner_task_paint == paint);

    const auto terminal = f.Tick(200);
    Sample(terminal, 0, Endpoint::Closed, Kind::Terminal);
    assert(terminal->display_list->commands == f.app.impl_->last_list->commands);
    assert(f.State().phase == Phase::Closing && f.app.impl_->owner_task_paint == paint);
    f.Submit(terminal, false);
    assert(f.State().phase == Phase::Closing && f.app.impl_->owner_task_paint == paint);
    f.Submit(terminal);
    assert(f.State().phase == Phase::Closed && !f.app.impl_->owner_task_paint);
    assert(!f.app.impl_->owner_task_motion && !f.app.impl_->HasActiveAnimations());
    assert(!f.app.impl_->animation_worker_active);
}

void CheckRefreshKeepsTheTrajectoryAndLastSeenSource()
{
    Fixture f;
    const auto identity = f.Begin(true);
    const auto seen = f.Tick(25);
    f.Submit(seen);
    const auto paint = f.app.impl_->owner_task_paint;
    const auto generation = seen->task_motion->generation;
    auto view = FileView();
    view.rows[0] = {"unadopted.txt", false, true};
    view.page_caption = "2 / 2";
    assert(f.app.UpdateOwnerFileTask(identity, view));
    const auto pending = f.Tick(50);
    Sample(pending, 0.5, Endpoint::Open, Kind::Intermediate);
    assert(pending->task_motion->generation == generation);
    assert(pending->task_presentation->projection > seen->task_presentation->projection);
    assert(f.app.impl_->owner_task_paint == paint);
    f.Submit(seen, true, runtime::SubmittedKind::State);
    assert(f.app.ActiveOwnerTask()->phase == runtime::TaskPhase::Preparing);
    assert(f.app.impl_->owner_task_motion->adopted == seen->task_motion);
    assert(f.app.CancelOwnerTask(identity));
    f.Take(identity, runtime::TaskOutcome::Cancelled);
    Sample(f.Current(), 0.25, Endpoint::Closed, Kind::Intermediate);
    assert(f.app.impl_->ClosingOwnerTaskPaint() == paint);
}

void CheckSuccessorRejectsTheRetiredMotion()
{
    Fixture f;
    const auto first = f.Begin();
    const auto seen = f.Tick(50);
    f.Submit(seen);
    assert(f.app.CancelOwnerTask(first));
    f.Take(first, runtime::TaskOutcome::Cancelled);
    const auto retired = f.Current();
    const auto second = f.Begin(true);
    const auto successor = f.Current();
    Sample(successor, 0, Endpoint::Open, Kind::Intermediate);
    assert(successor->task_presentation->identity.cycle >
           retired->task_presentation->identity.cycle);
    assert(!f.app.impl_->owner_task_paint);
    f.Submit(successor);
    const auto paint = f.app.impl_->owner_task_paint;
    f.Submit(retired, true, runtime::SubmittedKind::State);
    f.Submit(seen, true, runtime::SubmittedKind::State);
    assert(f.State().phase == Phase::Opening && f.State().identity.task == second);
    assert(f.app.impl_->owner_task_paint == paint);
    assert(f.app.impl_->owner_task_motion->adopted == successor->task_motion);
    const auto terminal = f.Tick(150);
    f.Submit(terminal);
    assert(f.State().phase == Phase::Open && f.State().identity.task == second);
}

enum class BadMotion { Missing, Identity, Generation, Revision, Kind, Endpoint, Time, Reveal };

void Corrupt(runtime::FramePacket &frame, BadMotion reason)
{
    switch (reason) {
    case BadMotion::Missing:
        frame.task_motion.reset();
        break;
    case BadMotion::Identity:
        ++frame.task_motion->identity.cycle;
        break;
    case BadMotion::Generation:
        ++frame.task_motion->generation;
        break;
    case BadMotion::Revision:
        ++frame.task_motion->revision;
        break;
    case BadMotion::Kind:
        frame.task_motion->sample.kind = Kind::Terminal;
        break;
    case BadMotion::Endpoint:
        frame.task_motion->sample.endpoint = Endpoint::Closed;
        break;
    case BadMotion::Time:
        frame.task_motion->sample.time_ns += 1'000 * millisecond;
        break;
    case BadMotion::Reveal:
        frame.task_motion->sample.reveal = std::numeric_limits<double>::quiet_NaN();
        break;
    }
}

void CheckMalformedMotionAssociationCannotBeAdopted()
{
    constexpr std::array reasons{BadMotion::Missing,  BadMotion::Identity, BadMotion::Generation,
                                 BadMotion::Revision, BadMotion::Kind,     BadMotion::Endpoint,
                                 BadMotion::Time,     BadMotion::Reveal};
    for (const auto reason : reasons) {
        Fixture f;
        f.Begin();
        const auto valid = f.Tick(40);
        auto bad = std::make_shared<runtime::FramePacket>(*valid);
        Corrupt(*bad, reason);
        f.Submit(bad, true, runtime::SubmittedKind::State);
        assert(f.State().phase == Phase::Opening && !f.State().adopted_sequence);
        assert(!f.app.impl_->owner_task_paint && !f.app.impl_->owner_task_motion->adopted);
        f.Submit(valid);
        assert(f.app.impl_->owner_task_motion->adopted == valid->task_motion);
        assert(f.app.impl_->owner_task_paint == valid->task_paint_candidate);
    }
}

void CheckThemeAndConfigureRetireOldSources()
{
    for (const bool closing : {false, true}) {
        for (const bool configure : {false, true}) {
            Fixture f;
            const auto identity = f.Begin(true);
            const auto old = f.Tick(25);
            f.Submit(old);
            if (closing) {
                assert(f.app.CancelOwnerTask(identity));
                f.Take(identity, runtime::TaskOutcome::Cancelled);
            }
            if (configure) {
                f.Resize();
            } else {
                assert(f.app.ApplyTheme(Theme("square", 2)));
            }
            assert(!f.app.impl_->owner_task_paint);
            f.Submit(old, true, runtime::SubmittedKind::State);
            assert(!f.app.impl_->owner_task_paint);
            if (closing) {
                assert(f.State().phase == Phase::Closed && !f.app.impl_->owner_task_motion);
                assert(!f.app.impl_->ClosingOwnerTaskPaint());
            } else {
                assert(f.app.ActiveOwnerTask()->identity == identity);
                const auto fresh = f.Tick(350);
                Sample(fresh, 1, Endpoint::Open, Kind::Terminal);
                f.Submit(fresh);
                assert(f.State().phase == Phase::Open);
                assert(f.app.impl_->owner_task_paint == fresh->task_paint_candidate);
            }
        }
    }
}

void CheckResourceEpochRejectsStaleFirstAdoptionAndClosingPaint()
{
    Fixture first;
    first.Begin();
    const auto pending = first.Tick(25);
    assert(first.app.impl_->commands.RegisterFont({77}, std::string(font_path)));
    first.Submit(pending, true, runtime::SubmittedKind::State);
    assert(first.State().phase == Phase::Opening && !first.State().adopted_sequence);
    assert(!first.app.impl_->owner_task_paint && !first.app.impl_->owner_task_motion->adopted);
    const auto fresh = first.Tick(50);
    assert(fresh->resource_epoch != pending->resource_epoch);
    first.Submit(fresh);
    assert(first.app.impl_->owner_task_motion->adopted == fresh->task_motion);

    Fixture closing;
    const auto identity = closing.Begin();
    closing.Submit(closing.Tick(50));
    assert(closing.app.CancelOwnerTask(identity));
    closing.Take(identity, runtime::TaskOutcome::Cancelled);
    const auto old = closing.Current();
    assert(closing.app.impl_->ClosingOwnerTaskPaint());
    assert(closing.app.impl_->commands.RegisterFont({77}, std::string(font_path)));
    assert(!closing.app.impl_->ClosingOwnerTaskPaint());
    const auto fallback = closing.Current();
    Sample(fallback, 0, Endpoint::Closed, Kind::Terminal);
    assert(!closing.app.impl_->owner_task_paint);
    closing.Submit(old, true, runtime::SubmittedKind::State);
    assert(!closing.app.impl_->owner_task_paint);
    closing.Submit(fallback);
    assert(closing.State().phase == Phase::Closed && !closing.app.impl_->owner_task_motion);
}

void CheckUiAndOwnerRetirementReleaseMotion()
{
    enum class Reason { Ui, Owner, Failure };
    for (const auto reason : {Reason::Ui, Reason::Owner, Reason::Failure}) {
        Fixture f;
        const auto identity = f.Begin();
        const auto seen = f.Tick(50);
        f.Submit(seen);
        assert(f.app.CancelOwnerTask(identity));
        f.Take(identity, runtime::TaskOutcome::Cancelled);
        assert(f.app.impl_->owner_task_motion && f.app.impl_->owner_task_paint);
        if (reason == Reason::Ui) {
            runtime::LoadDiagnostic diagnostic;
            assert(f.app.impl_->InstallScene(f.app.BeginUiLoad(), Master(), &diagnostic));
            f.Submit(seen, true, runtime::SubmittedKind::State);
        } else if (reason == Reason::Owner) {
            f.app.RetireOwnerTasks();
        } else {
            f.app.impl_->FailFrontend();
        }
        assert(f.State().phase == Phase::Closed);
        assert(!f.app.impl_->owner_task_paint && !f.app.impl_->owner_task_motion);
        assert(!f.app.impl_->ClosingOwnerTaskPaint());
    }
}

void CheckUnsupportedAndInstantProfilesStayImmediate()
{
    for (const auto profile : {Profile::Timed, Profile::Instant, Profile::Legacy}) {
        Fixture f{profile == Profile::Timed ? "glass" : "square", profile};
        if (profile == Profile::Legacy) {
            assert(f.app.impl_->theme->schema_version == 2);
            assert(f.app.impl_->theme->motion == contracts::MotionSet{});
        }
        const auto identity = f.Begin();
        const auto frame = f.Current();
        InstantEndpoint(frame, Endpoint::Open);
        if (profile == Profile::Timed) {
            assert(!frame->task_paint_candidate);
            assert(!f.app.impl_->owner_task_motion || f.app.impl_->owner_task_motion->fallback);
        }
        assert(!f.app.impl_->HasActiveAnimations());
        f.Click(runtime::kOwnerTaskChoiceActions[0], frame);
        assert(f.actions.empty());
        f.Submit(frame);
        assert(f.State().phase == Phase::Open);
        f.Click(runtime::kOwnerTaskChoiceActions[0], frame);
        assert(f.actions.size() == 1);
        assert(f.app.CancelOwnerTask(identity));
        f.Take(identity, runtime::TaskOutcome::Cancelled);
        const auto closed = f.Current();
        InstantEndpoint(closed, Endpoint::Closed);
        assert(!f.app.impl_->HasActiveAnimations());
        f.Submit(closed);
        assert(f.State().phase == Phase::Closed && !f.app.impl_->owner_task_motion);
    }
}

void CheckInitialUnsupportedDecisionLastsForTheCycle()
{
    for (const bool adopt_initial : {false, true}) {
        Fixture f;
        const auto root = f.Scene().RootId();
        assert(f.Scene().SetProperty(root, runtime::DslProperty::BackdropBlur, 8.0));
        assert(!runtime::Has(f.Scene().PendingDirty(), runtime::Dirty::Layout));
        assert(!runtime::Has(f.Scene().PendingDirty(), runtime::Dirty::Paint));
        const auto identity = f.Begin();
        const auto unsupported = f.Current();
        InstantEndpoint(unsupported, Endpoint::Open);
        assert(!unsupported->task_motion && !unsupported->task_paint_candidate);
        assert(!f.app.impl_->HasActiveAnimations());
        if (adopt_initial) {
            f.Submit(unsupported);
            assert(f.State().phase == Phase::Open);
        } else {
            assert(f.State().phase == Phase::Opening);
        }

        // Removing live blur alone does not rebuild the cached RenderTree.
        // The conservative export gate must keep the prior immediate decision.
        assert(f.Scene().SetProperty(root, runtime::DslProperty::BackdropBlur, 0.0));
        assert(!runtime::Has(f.Scene().PendingDirty(), runtime::Dirty::Layout));
        assert(!runtime::Has(f.Scene().PendingDirty(), runtime::Dirty::Paint));
        const auto conservative = f.Current();
        InstantEndpoint(conservative, Endpoint::Open);
        assert(!conservative->task_paint_candidate && !conservative->task_motion);
        assert(conservative->task_presentation->identity ==
               unsupported->task_presentation->identity);
        assert(!f.app.impl_->owner_task_motion && !f.app.impl_->HasActiveAnimations());

        // A real body update rebuilds drawing with the now-supported ancestor.
        // Input/projection may change, but this cycle cannot replay entrance.
        assert(f.app.SetBinding("document", std::string("Body rebuilt after backdrop removal")));
        const auto eligible = f.Current();
        InstantEndpoint(eligible, Endpoint::Open);
        assert(eligible->task_paint_candidate && !eligible->task_motion);
        assert(eligible->task_presentation->identity == unsupported->task_presentation->identity);
        assert(eligible->task_paint_candidate->source.projection ==
               eligible->task_presentation->projection);
        assert(eligible->pixels_revision > conservative->pixels_revision);
        assert(!f.app.impl_->owner_task_motion && !f.app.impl_->HasActiveAnimations());
        f.Submit(eligible);
        assert(f.State().phase == Phase::Open);
        assert(f.app.impl_->owner_task_paint == eligible->task_paint_candidate);
        f.Submit(unsupported, true, runtime::SubmittedKind::State);
        assert(f.State().phase == Phase::Open && !f.app.impl_->owner_task_motion);
        assert(f.app.impl_->owner_task_paint == eligible->task_paint_candidate);

        assert(f.app.CancelOwnerTask(identity));
        f.Take(identity, runtime::TaskOutcome::Cancelled);
        const auto closed = f.Current();
        InstantEndpoint(closed, Endpoint::Closed);
        assert(!closed->task_motion && !f.app.impl_->HasActiveAnimations());
        f.Submit(closed);
        assert(f.State().phase == Phase::Closed && !f.app.impl_->owner_task_paint);

        const auto next = f.Begin();
        const auto entrance = f.Current();
        Sample(entrance, 0, Endpoint::Open, Kind::Intermediate);
        assert(next != identity && entrance->task_paint_candidate);
        assert(entrance->task_presentation->identity.cycle >
               eligible->task_presentation->identity.cycle);
        assert(f.app.impl_->HasActiveAnimations());
    }
}

void CheckEarlyCancellationCannotResurrectOpening()
{
    Fixture f;
    const auto identity = f.Begin();
    const auto unadopted = f.Tick(75);
    assert(f.app.CancelOwnerTask(identity));
    f.Take(identity, runtime::TaskOutcome::Cancelled);
    assert(f.State().phase == Phase::Closed);
    assert(!f.app.impl_->owner_task_paint && !f.app.impl_->owner_task_motion);
    f.Submit(unadopted, true, runtime::SubmittedKind::State);
    assert(f.State().phase == Phase::Closed);
    assert(!f.app.impl_->owner_task_paint && !f.app.impl_->owner_task_motion);
    assert(!f.app.impl_->HasActiveAnimations());
}

void Run(std::string_view name, void (*check)())
{
    check();
    std::cout << "PASS " << name << '\n';
}
} // namespace

int main()
{
    Run("task-only motion uses unified opportunity", CheckTaskOnlyMotionUsesUnifiedOpportunity);
    Run("unchanged opportunity retains hard deadline", CheckIdleOpportunityKeepsHardDeadline);
    Run("opening business readiness and mouse gate differ",
        CheckOpeningBusinessReadyAndMouseGateAreSeparate);
    Run("intermediate adoption retains unmodulated paint",
        CheckIntermediateAdoptionRetainsUnmodulatedPaint);
    Run("earlier published intermediate still adopts",
        CheckDelayedEarlierIntermediateCanStillBeAdopted);
    Run("mid-opening close begins at actually adopted reveal",
        CheckMidOpeningCloseStartsFromAdoptedReveal);
    Run("closing retains readonly paint and current body",
        CheckClosingPaintIsReadonlyAndBodyStaysFresh);
    Run("refresh keeps motion and prior adopted source",
        CheckRefreshKeepsTheTrajectoryAndLastSeenSource);
    Run("successor rejects retired motion receipts", CheckSuccessorRejectsTheRetiredMotion);
    Run("malformed motion association cannot adopt",
        CheckMalformedMotionAssociationCannotBeAdopted);
    Run("theme and configure retire old source", CheckThemeAndConfigureRetireOldSources);
    Run("resource epoch rejects stale first and closing paint",
        CheckResourceEpochRejectsStaleFirstAdoptionAndClosingPaint);
    Run("UI and owner retirement release motion", CheckUiAndOwnerRetirementReleaseMotion);
    Run("unsupported, instant and legacy stay immediate",
        CheckUnsupportedAndInstantProfilesStayImmediate);
    Run("initial unsupported decision persists until a new cycle",
        CheckInitialUnsupportedDecisionLastsForTheCycle);
    Run("early cancellation cannot resurrect motion", CheckEarlyCancellationCannotResurrectOpening);
}
