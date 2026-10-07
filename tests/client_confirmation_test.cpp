#include "client_application_p.hpp"
#include "prism/contracts/display_list_validation.hpp"
#include "prism/runtime/load_plan.hpp"
#include "prism/runtime/owner_task_panel.hpp"
#include "prism/theme/compiler.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
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

std::string Read(const std::filesystem::path &path)
{
    std::ifstream input(path);
    assert(input);
    return {std::istreambuf_iterator<char>(input), {}};
}

sdk::ClientConfig Config()
{
    sdk::ClientConfig config;
    config.app_id = "confirmation-unit-fixture";
    config.font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    config.width = 640;
    config.height = 420;
    config.install_limits.nodes_per_turn = 32;
    config.install_limits.cpu_per_turn = std::chrono::milliseconds(10);
    return config;
}

runtime::PreparedComponent Master()
{
    const auto plan =
        runtime::CompileLoadPlan(R"(
Interface(version:2, layout:"ui/layout.prism") {
    Binding(name:"owner_label", type:"string", initial:"Current document")
    Component(id:"content", source:"ui/content.prism", phase:"critical")
    Component(id:"later", source:"ui/later.prism", phase:"deferred", after:["content"])
})",
                                 {"master", "/package/master.prism", "fixture-v1"}, "/package");
    const auto layout = runtime::PrepareLayout(R"(
Card(material:"window", padding:"@window_padding") {
    VStack(spacing:8) {
        Slot(component:"content", flex:1) { Text("Loading") }
        Slot(component:"later", height:32) { Text("Deferred pending") }
    }
})",
                                               plan);
    const std::array units{runtime::PreparedUnit{
        "content",
        runtime::PrepareComponent(R"(
VStack(spacing:8) {
    Text($owner_label, height:22, font:"@font_body", foreground:"@text")
    Button("Owner action", action:"owner", height:32)
    InteractionTarget(action:"owner-drag", height:48) {
        Visual(background:#123456FF)
    }.gesture(action:"owner-gesture", threshold:6)
    Card(flex:1)
})",
                                  {"content", "/package/ui/content.prism", "fixture-v1"})}};
    return runtime::ComposeCritical(plan, layout, units);
}

contracts::OwnerTaskRequest Request()
{
    contracts::OwnerTaskRequest request;
    request.request_id = 19;
    request.title = "Keep these changes?";
    request.message = "The current document has unsaved changes.\nChoose how to continue.";
    request.choices = {{11, "Discard", contracts::OwnerTaskChoiceRole::Destructive},
                       {42, "Save", contracts::OwnerTaskChoiceRole::Primary}};
    assert(contracts::ValidateOwnerTaskRequest(request));
    return request;
}

std::string RemoveLineBreaks(std::string_view text)
{
    std::string result;
    for (const char value : text) {
        if (value != '\n') {
            result.push_back(value == '\t' ? ' ' : value);
        }
    }
    return result;
}

template <std::size_t Count>
runtime::TextLayoutInfo VisibleLayout(const runtime::Scene &scene,
                                      const std::array<std::string_view, Count> &regions)
{
    for (const auto region : regions) {
        if (const auto metrics = scene.TextLayoutInRegion(region)) {
            assert(metrics->width > 0 && metrics->height > 0 && metrics->font_size > 0);
            return *metrics;
        }
    }
    assert(false && "No visible text measurement region");
    return {};
}

template <std::size_t Count>
contracts::NodeId ScrollParent(const runtime::Scene &scene,
                               const std::array<std::string_view, Count> &regions)
{
    const auto input = scene.InputGeometry();
    assert(input);
    for (const auto region : regions) {
        if (scene.TextLayoutInRegion(region)) {
            const auto *node = input->Find(scene.RegionId(region));
            assert(node && scene.ScrollInfo(node->parent));
            return node->parent;
        }
    }
    assert(false && "No visible task text ScrollView");
    return {};
}

const runtime::InputSnapshotNode &Action(const runtime::InputSnapshot &input,
                                         std::string_view action)
{
    for (const auto &node : input.nodes) {
        if (node.visible && node.action == action) {
            return node;
        }
    }
    assert(false && "Expected action is absent from the immutable input sample");
    return input.nodes.front();
}

bool ContainsGlyphs(const contracts::DisplayList &list, runtime::TextShaper &shaper,
                    std::string_view text, double font)
{
    const auto expected = shaper.Shape(text, font);
    assert(!expected.glyphs.empty());
    for (const auto &command : list.commands) {
        const auto *run = std::get_if<contracts::DrawGlyphRun>(&command);
        if (!run || run->font_size != font || run->glyphs.size() != expected.glyphs.size()) {
            continue;
        }
        bool matches = true;
        for (std::size_t index = 0; index < run->glyphs.size(); ++index) {
            if (run->glyphs[index].glyph_index != expected.glyphs[index].glyph_index) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return true;
        }
    }
    return false;
}

// This is a non-native unit fixture. It uses real DSL preparation, SDK
// composition/install policy, FreeType/HarfBuzz shaping and immutable frames.
// No Wayland connection, render worker, EGL/GPU or real submission is opened.
// Successful metadata below is simulated and is not native rendering evidence.
struct Fixture {
    sdk::ClientApplication app{Config()};
    std::shared_ptr<const runtime::FramePacket> shown;

    Fixture()
    {
        assert(app.FrontendReady());
        const auto theme = theme::LoadTheme(source_root / "resources/themes", "glass", 1);
        assert(app.ApplyTheme(theme));
        const auto panel =
            runtime::PrepareComponent(Read(source_root / "resources/ui/owner-task-panel.prism"));
        assert(app.ConfigureOwnerTaskPanel(panel));

        auto &impl = *app.impl_;
        impl.opened_once = true;
        impl.ui_configure_count = 1;
        impl.ui_metrics = {{640, 420}, {640, 420}, 1};
        impl.binding_values = {{"owner_label", std::string("Current document")}};
        const auto load = app.BeginUiLoad();
        runtime::LoadDiagnostic diagnostic;
        assert(impl.InstallScene(load, Master(), &diagnostic));
        assert(diagnostic.message.empty());
        assert(impl.installed_ui == load && !impl.render_owner && !impl.worker_generation);
        assert(runtime::HasOwnerTaskPanel(Scene()));
        assert(!Scene().IsVisible(Panel()));
        impl.PublishFramePacket();
        shown = impl.queued_frame;
        assert(shown && shown->display_list && shown->input_snapshot);
        Submit(shown, true);
        assert(app.SupportsOwnerConfirmation());
    }

    runtime::Scene &Scene()
    {
        assert(app.impl_->scene);
        return *app.impl_->scene;
    }

    contracts::NodeId Panel()
    {
        const auto region = Scene().RegionId(runtime::kOwnerTaskPanelRegion);
        assert(region && Scene().RegionMounted(runtime::kOwnerTaskPanelRegion));
        return region;
    }

    std::string Binding(std::string_view name) const
    {
        return std::get<std::string>(app.impl_->owner_task_bindings.at(std::string(name)));
    }

    void Submit(const std::shared_ptr<const runtime::FramePacket> &frame, bool prepared)
    {
        assert(frame && frame->display_list && frame->input_snapshot);
        contracts::ValidateDisplayList(*frame->display_list);
        runtime::SubmittedFrameEvent event;
        event.ui = frame->ui;
        event.frame_sequence = frame->sequence;
        event.frame = frame;
        event.scene_revision = frame->scene_revision;
        event.pixels_revision = frame->pixels_revision;
        event.theme_generation = frame->theme_generation;
        event.metadata_prepared = prepared;
        app.impl_->HandleSubmitted(event);
        assert(!app.impl_->failed);
        if (prepared) {
            shown = frame;
        }
    }

    runtime::TaskIdentity Begin(const contracts::OwnerTaskRequest &request = Request())
    {
        const auto old = shown;
        const auto identity = app.BeginOwnerConfirmation(request);
        assert(identity);
        assert(app.ActiveOwnerTask() ==
               (runtime::TaskEntry{*identity, runtime::TaskPhase::Preparing}));
        assert(app.impl_->owner_confirmation == request);
        assert(app.impl_->owner_task_scope && app.impl_->owner_task_scope->identity == identity);
        assert(Scene().IsVisible(Panel()) && Scene().OwnerModalToken());
        const auto frame = app.impl_->queued_frame;
        assert(frame && frame != old && frame->input_snapshot);
        assert(frame->display_list && frame->display_list != old->display_list);
        assert(frame->display_list->commands != old->display_list->commands);
        assert(frame->input_snapshot->owner_modal_epoch == Scene().OwnerModalToken());
        assert(Action(*frame->input_snapshot, runtime::kOwnerTaskCancelAction).interactive);
        assert(!Scene().IsInputSnapshotAdopted(*frame->input_snapshot));
        assert(!app.CompleteOwnerTask(*identity));
        return *identity;
    }

    void Ready(runtime::TaskIdentity identity)
    {
        const auto frame = app.impl_->queued_frame;
        assert(frame);
        Submit(frame, false);
        assert(app.ActiveOwnerTask()->phase == runtime::TaskPhase::Preparing);
        Submit(frame, true);
        assert(Scene().IsInputSnapshotAdopted(*frame->input_snapshot));
        assert(app.ActiveOwnerTask() == (runtime::TaskEntry{identity, runtime::TaskPhase::Ready}));
    }

    runtime::TaskTerminal Take(runtime::TaskIdentity identity, runtime::TaskOutcome outcome)
    {
        assert(!app.ActiveOwnerTask() && !app.impl_->owner_task_scope);
        assert(!Scene().OwnerModalToken() && !Scene().IsVisible(Panel()));
        assert(!app.impl_->owner_confirmation && app.impl_->owner_task_bindings.empty());
        const auto terminal = app.TakeOwnerTaskTerminal();
        assert(terminal && terminal->identity == identity && terminal->outcome == outcome);
        assert(!app.TakeOwnerTaskTerminal());
        return *terminal;
    }

    runtime::InteractionResult Click(std::string_view action,
                                     const std::shared_ptr<const runtime::InputSnapshot> &input)
    {
        const auto bounds = Action(*input, action).bounds;
        const contracts::LogicalPoint point{bounds.x + bounds.width / 2,
                                            bounds.y + bounds.height / 2};
        contracts::PointerButtonEvent down{window,
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
            shown->input_snapshot);
        impl.PublishFramePacket();
    }

    void CheckWrapped(std::string_view binding, std::string_view original,
                      const runtime::TextLayoutInfo &layout)
    {
        const auto text = Binding(binding);
        assert(RemoveLineBreaks(text) == RemoveLineBreaks(original));
        std::size_t start = 0;
        while (start < text.size()) {
            const auto end = text.find('\n', start);
            const auto line = std::string_view(text).substr(
                start, end == std::string::npos ? text.size() - start : end - start);
            const auto shaped = app.impl_->ShapeText(line, layout.font_size);
            assert(std::isfinite(shaped.width) && shaped.width <= layout.width + 0.001);
            assert(line.empty() || !shaped.glyphs.empty());
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }

    void CheckText(const contracts::OwnerTaskRequest &request)
    {
        CheckWrapped("__prism_task_title", request.title,
                     VisibleLayout(Scene(), runtime::kOwnerTaskTitleRegions));
        CheckWrapped("__prism_task_message", request.message,
                     VisibleLayout(Scene(), runtime::kOwnerTaskBodyRegions));
        for (std::size_t index = 0; index < request.choices.size(); ++index) {
            const auto layout =
                VisibleLayout(Scene(), runtime::kOwnerTaskChoiceLabelRegions[index]);
            const auto shape = app.impl_->ShapeText(request.choices[index].label, layout.font_size);
            assert(shape.width <= layout.width && shape.height <= layout.height);
        }
    }

    runtime::UiInstallState Advance(const runtime::BindingValues &bindings = {})
    {
        runtime::LoadDiagnostic diagnostic;
        runtime::UiInstallState state = runtime::UiInstallState::Pending;
        for (unsigned turn = 0; turn < 512 && state == runtime::UiInstallState::Pending; ++turn) {
            state = app.AdvanceUiInstall(bindings, &diagnostic);
        }
        assert(state != runtime::UiInstallState::Pending);
        return state;
    }
};

void CheckPreviewConfigurationAndBudgetMasterInstall()
{
    sdk::ClientApplication app(Config());
    assert(app.FrontendReady());
    assert(app.ApplyTheme(theme::LoadTheme(source_root / "resources/themes", "glass", 1)));
    auto &impl = *app.impl_;
    impl.opened_once = true;
    impl.ui_configure_count = 1;
    impl.ui_metrics = {{640, 420}, {640, 420}, 1};
    const auto preview_load = app.BeginUiLoad();
    assert(impl.InstallScene(preview_load,
                             runtime::PrepareComponent("HStack { Text(\"Preview\", height:32) }"),
                             nullptr));
    auto *const preview = impl.scene.get();
    assert(!runtime::HasOwnerTaskPanel(*preview) && !app.SupportsOwnerConfirmation());
    impl.PublishFramePacket();
    const auto old = impl.queued_frame;
    assert(old && old->display_list && old->input_snapshot);

    const auto panel =
        runtime::PrepareComponent(Read(source_root / "resources/ui/owner-task-panel.prism"));
    assert(app.ConfigureOwnerTaskPanel(panel));
    assert(!app.ConfigureOwnerTaskPanel(panel));
    assert(impl.scene.get() == preview && impl.installed_ui == preview_load);
    assert(!runtime::HasOwnerTaskPanel(*preview) && !app.SupportsOwnerConfirmation());
    assert(impl.queued_frame == old && !app.BeginOwnerConfirmation(Request()));

    const auto master_load = app.BeginUiLoad();
    assert(app.StartPreparedInstall(master_load, Master()));
    runtime::UiInstallState state = runtime::UiInstallState::Pending;
    for (unsigned turn = 0; turn < 512 && state == runtime::UiInstallState::Pending; ++turn) {
        state = app.AdvanceUiInstall({{"owner_label", std::string("Master document")}});
    }
    assert(state == runtime::UiInstallState::Committed && impl.installed_ui == master_load);
    assert(runtime::HasOwnerTaskPanel(*impl.scene) && app.SupportsOwnerConfirmation());
    const auto wrapper = impl.scene->RegionId(runtime::kOwnerTaskPanelRegion);
    assert(wrapper && !impl.scene->IsVisible(wrapper));
    assert(!impl.owner_confirmation && impl.owner_task_bindings.empty());
    assert(impl.queued_frame && impl.queued_frame->ui == master_load);
    assert(impl.install_stats.nodes > Master().NodeCount());
    for (const auto &[name, value] : impl.binding_values) {
        assert(!runtime::IsOwnerTaskReservedName(name));
    }
    assert(!impl.render_owner && !impl.worker_generation);
}

void CheckProjectionRenderAndAdoption()
{
    Fixture f;
    const auto request = Request();
    const auto identity = f.Begin(request);
    const auto frame = f.app.impl_->queued_frame;
    assert(ContainsGlyphs(*frame->display_list, f.app.impl_->shaper, request.title, 18));
    assert(ContainsGlyphs(*frame->display_list, f.app.impl_->shaper, "Discard", 14));
    assert(ContainsGlyphs(*frame->display_list, f.app.impl_->shaper, "Save", 14));
    f.CheckText(request);
    f.Ready(identity);

    assert(!f.Click("owner", frame->input_snapshot).activation);
    const auto choice = f.Click(runtime::kOwnerTaskChoiceActions[1], frame->input_snapshot);
    assert(choice.activation && choice.activation->action == runtime::kOwnerTaskChoiceActions[1]);
    assert(f.app.CompleteOwnerTask(identity));
    f.Take(identity, runtime::TaskOutcome::Success);
}

void CheckCancelHidesAndRejectsOldEpoch()
{
    Fixture f;
    const auto identity = f.Begin();
    f.Ready(identity);
    const auto old = f.shown;
    const auto token = f.Scene().OwnerModalToken();
    assert(f.app.CancelOwnerTask(identity));
    assert(f.Scene().OwnerModalEpoch() > token);
    assert(!f.Click(runtime::kOwnerTaskCancelAction, old->input_snapshot).activation);
    assert(!f.app.CompleteOwnerTask(identity));
    const auto cancelled = f.Take(identity, runtime::TaskOutcome::Cancelled);
    assert(cancelled.cancel_reason == runtime::TaskCancelReason::User);
    const auto hidden = f.app.impl_->queued_frame;
    assert(hidden && hidden != old &&
           hidden->display_list->commands != old->display_list->commands);
    assert(!ContainsGlyphs(*hidden->display_list, f.app.impl_->shaper, "Keep these changes?", 18));
    f.Submit(hidden, true);
    const auto owner = f.Click("owner", hidden->input_snapshot);
    assert(owner.activation && owner.activation->action == "owner");

    const auto next = f.Begin();
    f.Ready(next);
    assert(!f.app.CancelOwnerTask(identity));
    assert(f.app.CancelOwnerTask(next, runtime::TaskCancelReason::Escape));
    assert(f.Take(next, runtime::TaskOutcome::Cancelled).cancel_reason ==
           runtime::TaskCancelReason::Escape);
}

void CheckLongTextResizeAndTheme()
{
    Fixture f;
    auto request = Request();
    request.title = std::string(220, 'W');
    request.message =
        "First line stays explicit.\n\nThis paragraph contains a\ttab and enough words to wrap. ";
    for (unsigned index = 0; index < 18; ++index) {
        request.message += "The original document remains the working context. ";
    }
    request.message += "\nLast line remains present.";
    assert(contracts::ValidateOwnerTaskRequest(request));
    const auto identity = f.Begin(request);
    f.CheckText(request);
    f.Ready(identity);
    const auto token = f.Scene().OwnerModalToken();
    const auto wide_title = f.Binding("__prism_task_title");
    const auto wide_body = f.Binding("__prism_task_message");

    f.Resize(244, 204);
    assert(f.app.ActiveOwnerTask()->identity == identity && f.Scene().OwnerModalToken() == token);
    assert(f.Binding("__prism_task_title") != wide_title);
    assert(f.Binding("__prism_task_message") != wide_body);
    assert(f.Binding("__prism_task_message").find("First line stays explicit.\n\n") == 0);
    f.CheckText(request);

    // Font 16's destructive label needs more than the 56px available at 244px.
    // Keep this successful theme projection in the same narrow/short profile.
    f.Resize(320, 204);
    f.CheckText(request);
    auto theme = theme::LoadTheme(source_root / "resources/themes", "square", 2, "light");
    for (auto &number : theme.numbers) {
        if (number.name == "font_body") {
            number.value = 16;
        } else if (number.name == "font_title") {
            number.value = 20;
        }
    }
    assert(f.app.ApplyTheme(theme));
    assert(f.app.ThemeGeneration() == 2);
    assert(f.app.ActiveOwnerTask()->identity == identity && f.Scene().OwnerModalToken() == token);
    assert(VisibleLayout(f.Scene(), runtime::kOwnerTaskTitleRegions).font_size == 20);
    assert(VisibleLayout(f.Scene(), runtime::kOwnerTaskBodyRegions).font_size == 16);
    f.CheckText(request);
    f.Resize(640, 420);
    f.CheckText(request);

    f.Resize(500, 420);
    const auto previous_width = VisibleLayout(f.Scene(), runtime::kOwnerTaskBodyRegions).width;
    assert(f.Scene().SetProperty(f.Scene().InputGeometry()->root, runtime::DslProperty::Padding,
                                 32.0));
    assert(runtime::Has(f.Scene().PendingDirty(), runtime::Dirty::Layout));
    f.app.impl_->PublishFramePacket();
    assert(VisibleLayout(f.Scene(), runtime::kOwnerTaskBodyRegions).width < previous_width);
    f.CheckText(request);
    assert(f.app.ActiveOwnerTask()->identity == identity && f.Scene().OwnerModalToken() == token);
    assert(f.Scene().IsVisible(f.Panel()));
    assert(f.app.impl_->owner_confirmation == request);
    assert(f.app.CancelOwnerTask(identity));
    f.Take(identity, runtime::TaskOutcome::Cancelled);
}

void CheckNewRequestResetsTextScroll()
{
    Fixture f;
    auto request = Request();
    request.title = std::string(220, 'W');
    request.message.clear();
    for (unsigned line = 0; line < 35; ++line) {
        request.message += "Current document remains in view.\n";
    }
    assert(contracts::ValidateOwnerTaskRequest(request));
    const auto identity = f.Begin(request);
    f.Ready(identity);
    const auto body = ScrollParent(f.Scene(), runtime::kOwnerTaskBodyRegions);
    const auto title = ScrollParent(f.Scene(), runtime::kOwnerTaskTitleRegions);
    const auto body_info = f.Scene().ScrollInfo(body);
    const auto title_info = f.Scene().ScrollInfo(title);
    assert(body_info && body_info->maximum > 100);
    assert(title_info && title_info->maximum > 26);
    assert(f.Scene().ScrollTo(body, body_info->maximum));
    assert(f.Scene().ScrollTo(title, title_info->maximum));
    f.app.impl_->PublishFramePacket();
    f.Submit(f.app.impl_->queued_frame, true);
    assert(f.Scene().ScrollInfo(body)->offset == body_info->maximum);
    assert(f.Scene().ScrollInfo(title)->offset == title_info->maximum);

    assert(
        f.app.ApplyTheme(theme::LoadTheme(source_root / "resources/themes", "glass", 2, "light")));
    assert(f.Scene().ScrollInfo(body)->offset == body_info->maximum);
    assert(f.Scene().ScrollInfo(title)->offset == title_info->maximum);
    f.Resize(500, 420);
    assert(f.Scene().ScrollInfo(body)->offset == body_info->maximum);
    assert(f.Scene().ScrollInfo(title)->offset == title_info->maximum);
    assert(f.app.ActiveOwnerTask()->identity == identity);
    f.CheckText(request);
    f.Submit(f.app.impl_->queued_frame, true);

    assert(f.app.CancelOwnerTask(identity));
    f.Take(identity, runtime::TaskOutcome::Cancelled);
    f.Submit(f.app.impl_->queued_frame, true);
    ++request.request_id;
    const auto next = f.Begin(request);
    assert(next != identity);
    const auto next_body = ScrollParent(f.Scene(), runtime::kOwnerTaskBodyRegions);
    const auto next_title = ScrollParent(f.Scene(), runtime::kOwnerTaskTitleRegions);
    assert(f.Scene().ScrollInfo(next_body)->maximum > 100);
    assert(f.Scene().ScrollInfo(next_title)->maximum > 26);
    assert(f.Scene().ScrollInfo(next_body)->offset == 0);
    assert(f.Scene().ScrollInfo(next_title)->offset == 0);
    f.Ready(next);
    assert(f.app.CancelOwnerTask(next));
    f.Take(next, runtime::TaskOutcome::Cancelled);
}

void CheckUnreadableChoiceAndResizeFailure()
{
    Fixture rejected;
    auto request = Request();
    request.choices[0].label = std::string(96, 'W');
    assert(contracts::ValidateOwnerTaskRequest(request));
    bool threw = false;
    try {
        rejected.app.BeginOwnerConfirmation(request);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    assert(threw && !rejected.app.ActiveOwnerTask() && !rejected.app.impl_->owner_task_scope);
    assert(!rejected.app.impl_->owner_confirmation &&
           rejected.app.impl_->owner_task_bindings.empty());
    assert(!rejected.Scene().IsVisible(rejected.Panel()));
    assert(!rejected.app.TakeOwnerTaskTerminal());

    Fixture resized;
    const auto identity = resized.Begin();
    resized.Ready(identity);
    resized.Resize(70, 204);
    const auto failed = resized.Take(identity, runtime::TaskOutcome::Failed);
    assert(failed.failure && failed.failure->code == runtime::TaskFailureCode::PreparationFailed);
    assert(!failed.failure->diagnostic.empty());
    assert(!resized.app.SupportsOwnerConfirmation());
}

void CheckActiveRegionInstallKeepsTaskProjection()
{
    Fixture f;
    auto request = Request();
    request.title = "Deferred UI can finish while this request is open";
    request.message = "The task owns its projection.\nThe application owns its document.";
    const auto identity = f.Begin(request);
    f.Ready(identity);
    const auto token = f.Scene().OwnerModalToken();
    const auto panel = f.Panel();
    const auto title = f.Binding("__prism_task_title");
    const auto message = f.Binding("__prism_task_message");
    assert(!f.Scene().RegionMounted("later"));
    const std::array regions{runtime::PreparedRegion{
        "later", runtime::PrepareComponent("Text(\"Deferred ready\", font:\"@font_body\")")}};
    assert(f.app.StartRegionInstall(f.app.impl_->installed_ui, regions));
    assert(f.Advance({{"owner_label", std::string("Updated document")}}) ==
           runtime::UiInstallState::Committed);
    assert(f.Scene().RegionMounted("later") && f.Panel() == panel);
    assert(f.Scene().IsVisible(panel) && f.Scene().OwnerModalToken() == token);
    assert(f.app.ActiveOwnerTask() == (runtime::TaskEntry{identity, runtime::TaskPhase::Ready}));
    assert(f.Binding("__prism_task_title") == title);
    assert(f.Binding("__prism_task_message") == message);
    assert(std::get<bool>(f.app.impl_->owner_task_bindings.at("__prism_task_visible")));
    for (const auto &[name, value] : f.app.impl_->binding_values) {
        assert(!runtime::IsOwnerTaskReservedName(name));
    }
    f.CheckText(request);
    assert(ContainsGlyphs(*f.app.impl_->queued_frame->display_list, f.app.impl_->shaper,
                          "Deferred ready", 14));
    assert(f.app.CancelOwnerTask(identity));
    f.Take(identity, runtime::TaskOutcome::Cancelled);
}

void CheckReservedCallerProjectionRejected()
{
    Fixture f;
    const auto identity = f.Begin();
    f.Ready(identity);
    assert(!f.app.SetBinding("__prism_task_visible", false));
    assert(!f.app.SetBinding("__prism_task_title", std::string("Injected title")));
    const auto token = f.Scene().OwnerModalToken();
    const auto title = f.Binding("__prism_task_title");
    const auto message = f.Binding("__prism_task_message");
    const std::array regions{
        runtime::PreparedRegion{"later", runtime::PrepareComponent("Text(\"Deferred ready\")")}};
    assert(f.app.StartRegionInstall(f.app.impl_->installed_ui, regions));
    assert(f.Advance({{"__prism_task_visible", false},
                      {"__prism_task_message", std::string("Injected body")}}) ==
           runtime::UiInstallState::Failed);
    assert(!f.Scene().RegionMounted("later") && f.Scene().OwnerModalToken() == token);
    assert(f.Scene().IsVisible(f.Panel()) && f.app.ActiveOwnerTask()->identity == identity);
    assert(f.Binding("__prism_task_title") == title &&
           f.Binding("__prism_task_message") == message);
    assert(f.app.CancelOwnerTask(identity));
    f.Take(identity, runtime::TaskOutcome::Cancelled);
}

void CheckReservedSourcesRejectedWithoutTemplate()
{
    for (unsigned mode = 0; mode < 5; ++mode) {
        sdk::ClientApplication app(Config());
        assert(app.FrontendReady());
        auto &impl = *app.impl_;
        impl.opened_once = true;
        impl.ui_configure_count = 1;
        impl.ui_metrics = {{640, 420}, {640, 420}, 1};
        const auto old_load = app.BeginUiLoad();
        assert(impl.InstallScene(old_load, runtime::PrepareComponent(R"(
Card(padding:12) {
    VStack(spacing:8) {
        Text("Existing application", height:24)
        Button("Keep working", action:"owner", height:32)
    }
})"),
                                 nullptr));
        auto *const old_scene = impl.scene.get();
        impl.PublishFramePacket();
        const auto revision = old_scene->TransactionRevision();
        const auto old_frame = impl.queued_frame;
        assert(old_frame && !impl.owner_task_panel_template);
        assert(!runtime::HasOwnerTaskPanel(*old_scene) && !app.SupportsOwnerConfirmation());

        runtime::LoadDiagnostic diagnostic;
        if (mode < 4) {
            const auto bad = runtime::PrepareComponent(
                mode % 2 == 0 ? "Card { Button(\"Bad\", action:\"__prism_task_choice_0\") }"
                              : "Card { Text($__prism_task_message) }");
            const auto bad_load = app.BeginUiLoad();
            if (mode < 2) {
                assert(!impl.InstallScene(bad_load, bad, &diagnostic));
            } else {
                assert(app.StartPreparedInstall(bad_load, bad, &diagnostic));
                runtime::UiInstallState state = runtime::UiInstallState::Pending;
                for (unsigned turn = 0; turn < 512 && state == runtime::UiInstallState::Pending;
                     ++turn) {
                    state = app.AdvanceUiInstall({}, &diagnostic);
                }
                assert(state == runtime::UiInstallState::Failed);
            }
        } else {
            const std::array regions{runtime::PreparedRegion{
                "__prism_task_panel", runtime::PrepareComponent("Text(\"Bad region\")")}};
            if (app.StartRegionInstall(old_load, regions, &diagnostic)) {
                runtime::UiInstallState state = runtime::UiInstallState::Pending;
                for (unsigned turn = 0; turn < 512 && state == runtime::UiInstallState::Pending;
                     ++turn) {
                    state = app.AdvanceUiInstall({}, &diagnostic);
                }
                assert(state == runtime::UiInstallState::Failed);
            }
        }

        assert(!diagnostic.message.empty());
        assert(impl.scene.get() == old_scene && impl.installed_ui == old_load);
        assert(old_scene->TransactionRevision() == revision && impl.queued_frame == old_frame);
        assert(!app.UiInstallPending() && !impl.owner_task_panel_template);
        assert(!runtime::HasOwnerTaskPanel(*old_scene) && !app.ActiveOwnerTask());
        assert(!impl.render_owner && !impl.worker_generation);
    }
}

class CallbackFailure : public std::runtime_error {
public:
    CallbackFailure() : std::runtime_error("Confirmation fixture callback failed")
    {
    }
};

class ReentrantReceiver {
public:
    explicit ReentrantReceiver(sdk::ClientApplication &app) : app_(app)
    {
        app_.OnGesture(std::bind_front(&ReentrantReceiver::Handle, this));
    }

    ~ReentrantReceiver()
    {
        app_.OnGesture({});
    }

    void Handle(const contracts::GestureEvent &event)
    {
        assert(event.action == "owner-gesture");
        if (event.phase == contracts::GesturePhase::Begin) {
            gesture = event.id;
            return;
        }
        assert(event.phase == contracts::GesturePhase::Cancel && event.id == gesture);
        ++cancellations;
        const auto active = app_.ActiveOwnerTask();
        assert(active && active->phase == runtime::TaskPhase::Preparing);
        first = active->identity;
        assert(app_.CancelOwnerTask(*first));
        consumed = app_.TakeOwnerTaskTerminal();
        assert(consumed && consumed->identity == first);
        next_request = Request();
        ++next_request.request_id;
        next_request.title = "A newer request remains open";
        next = app_.BeginOwnerConfirmation(next_request);
        assert(next && next->owner == first->owner && next->request.value > first->request.value);
        throw CallbackFailure();
    }

    std::uint64_t gesture{};
    unsigned cancellations{};
    std::optional<runtime::TaskIdentity> first, next;
    std::optional<runtime::TaskTerminal> consumed;
    contracts::OwnerTaskRequest next_request;

private:
    sdk::ClientApplication &app_;
};

void CheckCallbackReentryPreservesNewerConfirmation()
{
    Fixture f;
    ReentrantReceiver receiver(f.app);
    const auto &target = Action(*f.shown->input_snapshot, "owner-drag");
    assert(target.gesture && target.gesture->action == "owner-gesture" &&
           target.gesture->threshold == 6);
    assert(f.Scene().IsInputSnapshotAdopted(*f.shown->input_snapshot));
    const auto bounds = target.bounds;
    const contracts::LogicalPoint point{bounds.x + bounds.width / 2, bounds.y + bounds.height / 2};
    f.app.impl_->HandleWindowEvent(
        contracts::PointerButtonEvent{window, point, contracts::PointerButton::Primary,
                                      contracts::ButtonState::Pressed, 0, 1, pointer, 781},
        f.shown->input_snapshot);
    f.app.impl_->HandleWindowEvent(
        contracts::PointerMotionEvent{window, {point.x + 10, point.y}, 2, pointer},
        f.shown->input_snapshot);
    assert(receiver.gesture && f.Scene().State(target.id).dragging);

    bool threw = false;
    try {
        f.app.BeginOwnerConfirmation(Request());
    } catch (const CallbackFailure &) {
        threw = true;
    }
    assert(threw && receiver.cancellations == 1 && receiver.first && receiver.next);
    assert(receiver.consumed && receiver.consumed->outcome == runtime::TaskOutcome::Cancelled);
    assert(f.app.ActiveOwnerTask() ==
           (runtime::TaskEntry{*receiver.next, runtime::TaskPhase::Preparing}));
    assert(f.app.impl_->owner_task_scope->identity == receiver.next);
    assert(f.app.impl_->owner_confirmation == receiver.next_request);
    assert(f.Scene().IsVisible(f.Panel()) && f.Scene().OwnerModalToken());
    assert(f.Binding("__prism_task_title") == receiver.next_request.title);
    assert(f.app.impl_->owner_confirmation_generation == 2);
    f.CheckText(receiver.next_request);
    f.Ready(*receiver.next);
    assert(!f.app.CancelOwnerTask(*receiver.first));
    assert(f.app.CancelOwnerTask(*receiver.next));
    f.Take(*receiver.next, runtime::TaskOutcome::Cancelled);
}

} // namespace

int main()
{
    CheckPreviewConfigurationAndBudgetMasterInstall();
    CheckProjectionRenderAndAdoption();
    CheckCancelHidesAndRejectsOldEpoch();
    CheckLongTextResizeAndTheme();
    CheckNewRequestResetsTextScroll();
    CheckUnreadableChoiceAndResizeFailure();
    CheckActiveRegionInstallKeepsTaskProjection();
    CheckReservedCallerProjectionRejected();
    CheckReservedSourcesRejectedWithoutTemplate();
    CheckCallbackReentryPreservesNewerConfirmation();
}
