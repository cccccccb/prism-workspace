#include "client_application_p.hpp"

#include "prism/contracts/display_list_validation.hpp"
#include "prism/runtime/owner_file_panel.hpp"
#include "prism/runtime/owner_task_panel.hpp"
#include "prism/runtime/task_paint.hpp"
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
#include <variant>
#include <vector>

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
constexpr std::string_view font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
using Phase = runtime::TaskPresentationPhase;

std::string Read(const std::filesystem::path &path)
{
    std::ifstream input(path);
    assert(input);
    return {std::istreambuf_iterator<char>(input), {}};
}

sdk::ClientConfig Config()
{
    sdk::ClientConfig config;
    config.app_id = "task-paint-sdk-fixture";
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

// Real shared DSL, shaping, Scene builds and immutable FramePackets. Receipts
// are deliberately controlled: there is no Wayland connection or render worker.
// The optional raster comparison is CPU evidence, not native GPU presentation.
struct Fixture {
    sdk::ClientApplication app{Config()};

    Fixture()
    {
        assert(app.FrontendReady());
        assert(
            app.ApplyTheme(InstantTheme(source_root / "resources/themes", "square", 1, "light")));
        assert(app.ConfigureOwnerTaskPanel(
            runtime::PrepareComponent(Read(source_root / "resources/ui/owner-task-panel.prism"))));
        assert(app.ConfigureOwnerFilePanel(
            runtime::PrepareComponent(Read(source_root / "resources/ui/owner-file-panel.prism"))));

        auto &impl = *app.impl_;
        impl.opened_once = true;
        impl.ui_configure_count = 1;
        impl.ui_metrics = {{640, 420}, {640, 420}, 1};
        impl.binding_values = {{"owner_width", 120.0},
                               {"document", std::string("Current document")}};
        Install();
        Submit(Current());
        assert(!impl.render_owner && !impl.worker_generation);
        assert(!impl.owner_task_paint && !impl.ClosingOwnerTaskPaint());
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
        assert(app.impl_->owner_task_scope && Scene().OwnerModalToken());
        assert(Current()->task_presentation);
        return *identity;
    }

    runtime::TaskPresentationState State()
    {
        const auto state = app.OwnerTaskPresentation();
        assert(state);
        return *state;
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

void CheckSource(const runtime::TaskPaintFragment &fragment, const runtime::FramePacket &frame)
{
    assert(frame.task_presentation);
    const auto &stamp = *frame.task_presentation;
    const auto &source = fragment.source;
    assert(source.identity == stamp.identity);
    assert(source.projection == stamp.projection);
    assert(source.frame_sequence == frame.sequence);
    assert(source.configure_count == frame.configure_count);
    assert(source.buffer_size.width == frame.buffer_size.width);
    assert(source.buffer_size.height == frame.buffer_size.height);
    assert(source.scale == frame.scale);
    assert(source.theme_generation == frame.theme_generation);
    assert(source.resource_epoch == frame.resource_epoch);

    contracts::DisplayList list{window, frame.sequence, fragment.commands};
    contracts::ValidateDisplayList(list);
    assert(!list.commands.empty());
    assert(std::none_of(list.commands.begin(), list.commands.end(), [](const auto &command) {
        return std::holds_alternative<contracts::DrawImage>(command);
    }));
    assert(std::any_of(list.commands.begin(), list.commands.end(), [](const auto &command) {
        const auto *glyphs = std::get_if<contracts::DrawGlyphRun>(&command);
        return glyphs && !glyphs->glyphs.empty() && glyphs->font;
    }));
}

void CheckPreparationDoesNotBecomeAuthority()
{
    for (const bool file : {false, true}) {
        Fixture f;
        f.Begin(file);
        const auto frame = f.Current();
        assert(frame->task_paint_candidate);
        CheckSource(*frame->task_paint_candidate, *frame);
        assert(frame->task_paint_candidate->commands != frame->display_list->commands);
        assert(!f.app.impl_->owner_task_paint && !f.app.impl_->ClosingOwnerTaskPaint());
        assert(f.State().phase == Phase::Opening);
        f.Submit(frame, false);
        assert(!f.app.impl_->owner_task_paint && f.State().phase == Phase::Opening);
        f.Submit(frame, true, runtime::SubmittedKind::State);
        assert(f.State().phase == Phase::Open);
        assert(f.app.impl_->owner_task_paint == frame->task_paint_candidate);
        assert(!f.app.impl_->ClosingOwnerTaskPaint());
    }
}

void CheckStaleProjectionCannotReplaceAuthority()
{
    Fixture f;
    const auto identity = f.Begin(true);
    const auto first = f.Current();
    f.Submit(first);
    const auto old = f.app.impl_->owner_task_paint;
    assert(old == first->task_paint_candidate);

    auto view = FileView();
    view.rows[0] = {"new projection.txt", false, true};
    assert(f.app.UpdateOwnerFileTask(identity, view));
    const auto next = f.Current();
    assert(next->task_paint_candidate && next->task_paint_candidate != old);
    assert(next->task_paint_candidate->commands != old->commands);
    assert(next->task_paint_candidate->source.projection > old->source.projection);
    assert(f.app.impl_->owner_task_paint == old);
    f.Submit(first);
    f.Submit(next, false);
    assert(f.app.impl_->owner_task_paint == old);
    f.Submit(next);
    assert(f.app.impl_->owner_task_paint == next->task_paint_candidate);
}

enum class BadSource {
    Task,
    Ui,
    Cycle,
    Projection,
    Sequence,
    Configure,
    Buffer,
    Scale,
    Theme,
    Resources
};

void Corrupt(runtime::TaskPaintSource &source, BadSource reason)
{
    switch (reason) {
    case BadSource::Task:
        ++source.identity.task.owner.value;
        break;
    case BadSource::Ui:
        ++source.identity.ui.generation;
        break;
    case BadSource::Cycle:
        ++source.identity.cycle;
        break;
    case BadSource::Projection:
        ++source.projection;
        break;
    case BadSource::Sequence:
        ++source.frame_sequence;
        break;
    case BadSource::Configure:
        ++source.configure_count;
        break;
    case BadSource::Buffer:
        ++source.buffer_size.width;
        break;
    case BadSource::Scale:
        source.scale = 2;
        break;
    case BadSource::Theme:
        ++source.theme_generation;
        break;
    case BadSource::Resources:
        ++source.resource_epoch;
        break;
    }
}

void CheckFragmentAssociationIsValidated()
{
    constexpr std::array reasons{BadSource::Task,       BadSource::Ui,       BadSource::Cycle,
                                 BadSource::Projection, BadSource::Sequence, BadSource::Configure,
                                 BadSource::Buffer,     BadSource::Scale,    BadSource::Theme,
                                 BadSource::Resources};
    for (const auto reason : reasons) {
        Fixture f;
        f.Begin();
        const auto valid = f.Current();
        assert(valid->task_paint_candidate);
        auto bad = std::make_shared<runtime::FramePacket>(*valid);
        auto fragment = std::make_shared<runtime::TaskPaintFragment>(*valid->task_paint_candidate);
        Corrupt(fragment->source, reason);
        bad->task_paint_candidate = std::move(fragment);
        f.Submit(bad);
        assert(f.State().phase == Phase::Open);
        assert(!f.app.impl_->owner_task_paint);
    }
}

void CheckResourceUnsafeCandidatesCannotBecomeAuthority()
{
    for (const bool image : {false, true}) {
        Fixture f;
        f.Begin();
        const auto valid = f.Current();
        auto bad = std::make_shared<runtime::FramePacket>(*valid);
        auto fragment = std::make_shared<runtime::TaskPaintFragment>(*valid->task_paint_candidate);
        if (image) {
            fragment->commands.emplace_back(contracts::DrawImage{{99}, {0, 0, 24, 24}});
        } else {
            const auto glyph = std::find_if(
                fragment->commands.begin(), fragment->commands.end(), [](const auto &command) {
                    return std::holds_alternative<contracts::DrawGlyphRun>(command);
                });
            assert(glyph != fragment->commands.end());
            std::get<contracts::DrawGlyphRun>(*glyph).font = {99};
        }
        bad->task_paint_candidate = std::move(fragment);
        f.Submit(bad);
        assert(f.State().phase == Phase::Open && !f.app.impl_->owner_task_paint);
    }
}

void CheckCloseSelectsTheLastAdoptedProjection()
{
    Fixture f;
    const auto identity = f.Begin(true);
    const auto shown = f.Current();
    f.Submit(shown);
    const auto adopted = f.app.impl_->owner_task_paint;
    const auto saved_commands = adopted->commands;
    auto view = FileView();
    view.filename = "unadopted.txt";
    view.rows[0] = {"unadopted.txt", false, true};
    assert(f.app.UpdateOwnerFileTask(identity, view));
    const auto pending = f.Current();
    assert(pending->task_paint_candidate->commands != saved_commands);
    assert(f.app.CancelOwnerTask(identity));
    f.Take(identity, runtime::TaskOutcome::Cancelled);
    assert(f.State().phase == Phase::Closing);
    assert(f.app.impl_->ClosingOwnerTaskPaint() == adopted);
    assert(adopted->commands == saved_commands);
    f.Submit(pending);
    assert(f.app.impl_->ClosingOwnerTaskPaint() == adopted);

    const auto hidden = f.Current();
    assert(!hidden->task_paint_candidate);
    assert(hidden->task_presentation->endpoint == runtime::TaskPresentationEndpoint::Closed);
    f.Submit(hidden, false);
    assert(f.app.impl_->ClosingOwnerTaskPaint() == adopted);
    f.Submit(hidden);
    assert(f.State().phase == Phase::Closed);
    assert(!f.app.impl_->owner_task_paint && !f.app.impl_->ClosingOwnerTaskPaint());
    // Value ownership outlives cache retirement without accessing Scene state.
    assert(adopted->commands == saved_commands);
}

void CheckAcceptedUnsupportedProjectionClearsOldPaint()
{
    Fixture f;
    const auto identity = f.Begin(true);
    const auto shown = f.Current();
    f.Submit(shown);
    assert(f.app.impl_->owner_task_paint);
    auto view = FileView();
    view.status = "Latest projection has no safe paint export";
    assert(f.app.UpdateOwnerFileTask(identity, view));
    auto fallback = std::make_shared<runtime::FramePacket>(*f.Current());
    // A controlled missing export models a valid adopted projection whose paint
    // dependencies are unsupported. Its old projection must not survive close.
    fallback->task_paint_candidate.reset();
    f.Submit(fallback);
    assert(f.State().phase == Phase::Open && !f.app.impl_->owner_task_paint);
    assert(f.app.CancelOwnerTask(identity));
    f.Take(identity, runtime::TaskOutcome::Cancelled);
    assert(!f.app.impl_->ClosingOwnerTaskPaint());
    f.Submit(f.Current());
    assert(f.State().phase == Phase::Closed);
}

runtime::InteractionResult Click(Fixture &f, std::string_view action,
                                 const std::shared_ptr<const runtime::InputSnapshot> &input)
{
    const auto target =
        std::find_if(input->nodes.begin(), input->nodes.end(),
                     [action](const auto &node) { return node.visible && node.action == action; });
    assert(target != input->nodes.end());
    const contracts::LogicalPoint point{target->bounds.x + target->bounds.width / 2,
                                        target->bounds.y + target->bounds.height / 2};
    const contracts::PointerButtonEvent down{
        window,  point, contracts::PointerButton::Primary, contracts::ButtonState::Pressed, 0, 1,
        pointer, 781};
    auto up = down;
    up.state = contracts::ButtonState::Released;
    f.Scene().HandleInput(down, input);
    return f.Scene().HandleInput(up, input);
}

void CheckZeroDurationRetiresInputAndKeepsBodyFresh()
{
    Fixture f;
    const auto identity = f.Begin();
    const auto shown = f.Current();
    f.Submit(shown);
    const auto adopted = f.app.impl_->owner_task_paint;
    const auto saved_commands = adopted->commands;
    assert(f.app.CompleteOwnerTask(identity));
    f.Take(identity, runtime::TaskOutcome::Success);
    assert(!Click(f, runtime::kOwnerTaskChoiceActions[0], shown->input_snapshot).activation);
    const auto hidden = f.Current();
    assert(!hidden->task_paint_candidate);
    assert(!f.Scene().IsVisible(f.Scene().RegionId(runtime::kOwnerTaskPanelRegion)));
    const auto first_pixels = f.Raster(*hidden->display_list);

    assert(f.app.SetBinding("document", std::string("Document changed while task closes")));
    assert(f.app.SetBinding("owner_width", 224.0));
    const auto current = f.Current();
    assert(current->sequence > hidden->sequence);
    assert(current->display_list->commands != hidden->display_list->commands);
    assert(f.Raster(*current->display_list) != first_pixels);
    assert(!current->task_paint_candidate && !f.Scene().OwnerModalToken());
    assert(current->input_snapshot->owner_modal_epoch == f.Scene().OwnerModalEpoch());
    assert(current->input_snapshot->owner_modal_epoch > shown->input_snapshot->owner_modal_epoch);
    assert(adopted->commands == saved_commands);
    assert(f.app.impl_->ClosingOwnerTaskPaint() == adopted);
    f.Submit(hidden);
    assert(f.State().phase == Phase::Closing && f.app.impl_->owner_task_paint == adopted);
    f.Submit(current);
    assert(f.State().phase == Phase::Closed && !f.app.impl_->owner_task_paint);
    assert(Click(f, "owner", current->input_snapshot).activation);
}

void CheckSuccessorIgnoresTheRetiredCycle()
{
    Fixture f;
    const auto first = f.Begin();
    const auto shown = f.Current();
    f.Submit(shown);
    const auto old = f.app.impl_->owner_task_paint;
    assert(f.app.CompleteOwnerTask(first));
    f.Take(first, runtime::TaskOutcome::Success);
    const auto closing = f.Current();
    assert(f.app.impl_->ClosingOwnerTaskPaint() == old);
    const auto second = f.Begin(true);
    const auto successor = f.Current();
    assert(successor->task_paint_candidate);
    assert(!f.app.impl_->owner_task_paint && !f.app.impl_->ClosingOwnerTaskPaint());
    assert(successor->task_paint_candidate->source.identity.cycle > old->source.identity.cycle);
    f.Submit(successor);
    const auto current = f.app.impl_->owner_task_paint;
    assert(current == successor->task_paint_candidate);
    f.Submit(closing);
    f.Submit(shown);
    assert(f.app.impl_->owner_task_paint == current);
    assert(f.State().identity.task == second && f.State().phase == Phase::Open);
}

void CheckThemeAndGeometryRetirePaint()
{
    for (const bool closing : {false, true}) {
        for (const bool geometry : {false, true}) {
            Fixture f;
            const auto identity = f.Begin(true);
            const auto shown = f.Current();
            f.Submit(shown);
            assert(f.app.impl_->owner_task_paint);
            if (closing) {
                assert(f.app.CancelOwnerTask(identity));
                f.Take(identity, runtime::TaskOutcome::Cancelled);
                assert(f.app.impl_->ClosingOwnerTaskPaint());
            }

            if (geometry) {
                f.Resize();
            } else {
                assert(f.app.ApplyTheme(
                    InstantTheme(source_root / "resources/themes", "square", 2, "dark")));
            }
            assert(!f.app.impl_->owner_task_paint && !f.app.impl_->ClosingOwnerTaskPaint());
            f.Submit(shown);
            assert(!f.app.impl_->owner_task_paint);
            if (closing) {
                assert(f.State().phase == Phase::Closed);
            } else {
                const auto changed = f.Current();
                assert(changed->task_paint_candidate);
                f.Submit(changed);
                assert(f.app.impl_->owner_task_paint == changed->task_paint_candidate);
                assert(f.State().identity.task == identity && f.State().phase == Phase::Open);
            }
        }
    }
}

void CheckResourceEpochFence()
{
    Fixture initial;
    initial.Begin();
    const auto pending = initial.Current();
    assert(initial.State().phase == Phase::Opening && pending->task_paint_candidate);
    assert(initial.app.impl_->commands.RegisterFont({76}, std::string(font_path)));
    // The first receipt can carry valid input geometry but a retired drawing
    // environment. Publish no replacement before observing this actual gate.
    initial.Submit(pending);
    assert(initial.State().phase == Phase::Opening);
    assert(!initial.State().adopted_sequence && !initial.app.impl_->owner_task_paint);
    const auto fresh = initial.Current();
    assert(fresh->sequence > pending->sequence && fresh->task_paint_candidate);
    assert(fresh->resource_epoch != pending->resource_epoch);
    initial.Submit(fresh);
    assert(initial.State().phase == Phase::Open);
    assert(initial.app.impl_->owner_task_paint == fresh->task_paint_candidate);

    for (const bool closing : {false, true}) {
        Fixture f;
        const auto identity = f.Begin();
        const auto shown = f.Current();
        f.Submit(shown);
        const auto previous = f.app.impl_->owner_task_paint;
        if (closing) {
            assert(f.app.CancelOwnerTask(identity));
            f.Take(identity, runtime::TaskOutcome::Cancelled);
            assert(f.app.impl_->ClosingOwnerTaskPaint() == previous);
        }

        assert(f.app.impl_->commands.RegisterFont({77}, std::string(font_path)));
        assert(f.app.impl_->commands.ResourceEpoch() != shown->resource_epoch);
        assert(!f.app.impl_->ClosingOwnerTaskPaint());
        const auto changed = f.Current();
        assert(!f.app.impl_->owner_task_paint);
        f.Submit(shown);
        assert(!f.app.impl_->owner_task_paint);
        f.Submit(changed);
        if (closing) {
            assert(f.State().phase == Phase::Closed);
            assert(!f.app.impl_->owner_task_paint);
        } else {
            assert(changed->task_paint_candidate);
            assert(f.app.impl_->owner_task_paint == changed->task_paint_candidate);
            assert(f.app.impl_->owner_task_paint->source.resource_epoch !=
                   previous->source.resource_epoch);
        }
    }
}

void CheckUiReplacementAndRetirementReleaseCache()
{
    enum class Reason { Ui, Owner, Failure };
    for (const auto reason : {Reason::Ui, Reason::Owner, Reason::Failure}) {
        for (const bool closing : {false, true}) {
            Fixture f;
            const auto identity = f.Begin();
            const auto shown = f.Current();
            f.Submit(shown);
            const auto owned = f.app.impl_->owner_task_paint;
            const auto copied = owned->commands;
            if (closing) {
                assert(f.app.CancelOwnerTask(identity));
                f.Take(identity, runtime::TaskOutcome::Cancelled);
            }
            if (reason == Reason::Ui) {
                f.Install();
                f.Submit(shown);
            } else if (reason == Reason::Owner) {
                f.app.RetireOwnerTasks();
            } else {
                f.app.impl_->FailFrontend();
            }
            assert(!f.app.impl_->owner_task_paint && !f.app.impl_->ClosingOwnerTaskPaint());
            assert(f.State().phase == Phase::Closed);
            assert(owned->commands == copied);
            assert(!f.app.ActiveOwnerTask());
        }
    }
}

void CheckGenericAndUnadoptedTasksHaveNoRetainedPaint()
{
    Fixture generic;
    auto blueprint = runtime::ParseBlueprint(R"(
Card {
    Text("Application-owned task", height:30)
    Button("Finish", action:"generic", height:32)
})");
    blueprint.region = "generic";
    blueprint.region_mounted = true;
    auto &impl = *generic.app.impl_;
    impl.scene = std::make_unique<runtime::Scene>(
        std::move(blueprint), std::bind_front(&sdk::ClientApplication::Impl::ShapeText, &impl),
        impl.commands.FontId(), impl.theme);
    assert(generic.Scene().SetViewport({640, 420}));
    generic.Submit(generic.Current());
    const auto identity = generic.app.BeginOwnerTask("generic");
    assert(identity);
    const auto shown = generic.Current();
    assert(shown->task_presentation && !shown->task_paint_candidate);
    generic.Submit(shown);
    assert(generic.State().phase == Phase::Open && !impl.owner_task_paint);
    assert(generic.app.CancelOwnerTask(*identity));
    generic.Take(*identity, runtime::TaskOutcome::Cancelled);
    assert(!impl.ClosingOwnerTaskPaint());
    generic.Submit(generic.Current());
    assert(generic.State().phase == Phase::Closed);

    Fixture early;
    const auto request = early.Begin();
    const auto candidate = early.Current();
    assert(candidate->task_paint_candidate && !early.app.impl_->owner_task_paint);
    assert(early.app.CancelOwnerTask(request));
    early.Take(request, runtime::TaskOutcome::Cancelled);
    assert(early.State().phase == Phase::Closed);
    early.Submit(candidate);
    assert(!early.app.impl_->owner_task_paint && !early.app.impl_->ClosingOwnerTaskPaint());
}

void CheckBackdropUsesImmediateFallback()
{
    for (const bool file : {false, true}) {
        Fixture f;
        assert(
            f.app.ApplyTheme(InstantTheme(source_root / "resources/themes", "glass", 2, "dark")));
        const auto identity = f.Begin(file);
        const auto shown = f.Current();
        assert(shown->task_presentation && !shown->task_paint_candidate);
        f.Submit(shown);
        assert(f.State().phase == Phase::Open && !f.app.impl_->owner_task_paint);
        assert(f.app.CancelOwnerTask(identity));
        f.Take(identity, runtime::TaskOutcome::Cancelled);
        assert(!f.app.impl_->ClosingOwnerTaskPaint());
        f.Submit(f.Current());
        assert(f.State().phase == Phase::Closed);
    }
}

void CheckUnchangedThemeAndDiscardedUiLoadKeepAuthority()
{
    Fixture f;
    f.Begin();
    const auto shown = f.Current();
    f.Submit(shown);
    const auto adopted = f.app.impl_->owner_task_paint;
    const auto state = f.State();
    assert(f.app.ApplyTheme(*f.app.impl_->theme));
    f.app.BeginUiLoad();
    f.app.CancelUiLoad();
    assert(f.State() == state);
    assert(f.app.impl_->owner_task_paint == adopted);
    assert(!f.app.impl_->ClosingOwnerTaskPaint());
}

void Run(std::string_view name, void (*check)())
{
    check();
    std::cout << "PASS " << name << '\n';
}
} // namespace

int main()
{
    Run("preparation has no adopted authority", CheckPreparationDoesNotBecomeAuthority);
    Run("stale projection cannot replace adopted paint",
        CheckStaleProjectionCannotReplaceAuthority);
    Run("fragment source association fences", CheckFragmentAssociationIsValidated);
    Run("resource-unsafe candidates cannot become authority",
        CheckResourceUnsafeCandidatesCannotBecomeAuthority);
    Run("close selects last adopted projection", CheckCloseSelectsTheLastAdoptedProjection);
    Run("unsupported adopted projection clears old paint",
        CheckAcceptedUnsupportedProjectionClearsOldPaint);
    Run("zero duration closes input and keeps current body",
        CheckZeroDurationRetiresInputAndKeepsBodyFresh);
    Run("successor ignores retired cycle receipts", CheckSuccessorIgnoresTheRetiredCycle);
    Run("theme and geometry release retained paint", CheckThemeAndGeometryRetirePaint);
    Run("resource epoch releases retained paint", CheckResourceEpochFence);
    Run("UI replacement and owner retirement release cache",
        CheckUiReplacementAndRetirementReleaseCache);
    Run("generic and early cancelled tasks retain no paint",
        CheckGenericAndUnadoptedTasksHaveNoRetainedPaint);
    Run("backdrop remains immediate fallback", CheckBackdropUsesImmediateFallback);
    Run("unchanged theme and discarded candidate preserve authority",
        CheckUnchangedThemeAndDiscardedUiLoadKeepAuthority);
}
