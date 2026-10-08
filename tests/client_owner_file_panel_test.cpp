#include "client_application_p.hpp"

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
#include <optional>
#include <stdexcept>

namespace {
using namespace prism;
const std::filesystem::path root{PRISM_SOURCE_ROOT};
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

runtime::PreparedComponent Panel(std::string_view name)
{
    std::ifstream input(root / "resources/ui" / name);
    assert(input);
    return runtime::PrepareComponent(std::string{std::istreambuf_iterator<char>(input), {}});
}

sdk::ClientConfig Config()
{
    sdk::ClientConfig config;
    config.app_id = "file-panel-fixture";
    config.font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    config.width = 640;
    config.height = 420;
    return config;
}

contracts::OwnerTaskRequest Request()
{
    contracts::OwnerTaskRequest request;
    request.request_id = 7;
    request.kind = contracts::OwnerTaskKind::SaveFile;
    request.title = "Save file";
    request.file = contracts::OwnerFileTaskOptions{"/tmp", "notes.txt", {".txt"}, false};
    assert(contracts::ValidateOwnerTaskRequest(request));
    return request;
}

runtime::OwnerFilePanelView View()
{
    runtime::OwnerFilePanelView view;
    view.title = "Save file";
    view.directory = "/tmp";
    view.filename = "notes.txt";
    view.status = "Select a save location";
    view.page_caption = "1 / 1";
    view.selected_caption = "/tmp/" + std::string(251, 'x') + ".txt";
    view.rows[0] = {"notes.txt", false, true};
    view.nav_enabled = true;
    view.submit_enabled = true;
    view.show_filename = true;
    return view;
}

// Real DSL and SDK text shaping without a native window, render worker or compositor.
struct Fixture {
    sdk::ClientApplication app{Config()};
    std::shared_ptr<const runtime::InputSnapshot> shown;

    explicit Fixture(std::optional<contracts::Contour> clip_contour = std::nullopt)
    {
        assert(app.FrontendReady());
        assert(app.ConfigureOwnerTaskPanel(Panel("owner-task-panel.prism")));
        assert(app.ConfigureOwnerFilePanel(Panel("owner-file-panel.prism")));
        auto &impl = *app.impl_;
        auto blueprint = runtime::ParseBlueprint(R"(
Card(material:"window", padding:14) {
    Button("Document", action:"outside", height:32)
})");
        if (clip_contour) {
            std::erase_if(blueprint.properties, [](const auto &property) {
                return property.id == runtime::DslProperty::Material;
            });
            blueprint.properties.push_back({runtime::DslProperty::Clip, true});
            blueprint.contour = std::move(clip_contour);
        }
        const auto theme = InstantTheme(root / "resources/themes", "glass", 1);
        impl.theme = theme;
        impl.scene = std::make_unique<runtime::Scene>(
            impl.ComposeOwnerPanels(std::move(blueprint)),
            std::bind_front(&sdk::ClientApplication::Impl::ShapeText, &impl), impl.shaper.FontId(),
            theme);
        impl.scene->SetViewport({640, 420});
        assert(impl.scene->PrepareDetached(impl.OwnerPanelDefaults()));
        impl.opened_once = true;
        impl.installed_ui = impl.ui_load.Begin();
        impl.ui_configure_count = 1;
        impl.ui_metrics = {{640, 420}, {640, 420}, 1};
        shown = Capture();
        impl.scene->ApplyInputSnapshot(shown);
        assert(app.SupportsOwnerFileTasks() && app.SupportsOwnerConfirmation());
    }

    runtime::Scene &Scene()
    {
        return *app.impl_->scene;
    }

    std::shared_ptr<const runtime::InputSnapshot> Capture()
    {
        app.impl_->PublishFramePacket();
        const auto frame = app.impl_->queued_frame;
        assert(frame && frame->input_snapshot && frame->display_list);
        return frame->input_snapshot;
    }

    runtime::TaskIdentity Begin(const runtime::OwnerFilePanelView &view)
    {
        const auto identity = app.BeginOwnerFileTask(Request(), view);
        assert(identity);
        assert(app.ActiveOwnerTask()->phase == runtime::TaskPhase::Preparing);
        return *identity;
    }

    void Ready(runtime::TaskIdentity identity)
    {
        shown = Capture();
        const auto frame = app.impl_->queued_frame;
        runtime::SubmittedFrameEvent event;
        event.ui = frame->ui;
        event.frame_sequence = frame->sequence;
        event.frame = frame;
        event.scene_revision = frame->scene_revision;
        event.pixels_revision = frame->pixels_revision;
        event.theme_generation = frame->theme_generation;
        event.metadata_prepared = true;
        app.impl_->HandleSubmitted(event);
        assert(!app.impl_->failed && Scene().IsInputSnapshotAdopted(*shown));
        assert(frame->task_presentation && frame->task_presentation->sample_kind ==
                                               runtime::TaskPresentationSampleKind::Terminal);
        assert(app.OwnerTaskPresentation()->phase == runtime::TaskPresentationPhase::Open);
        assert(app.ActiveOwnerTask() == (runtime::TaskEntry{identity, runtime::TaskPhase::Ready}));
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
            shown);
        impl.PublishFramePacket();
    }

    void CheckFailedAndHidden(runtime::TaskIdentity identity)
    {
        assert(!app.ActiveOwnerTask() && !app.impl_->owner_task_scope);
        assert(!app.impl_->owner_file_view && !app.impl_->owner_file_kind);
        assert(app.impl_->owner_task_bindings.empty());
        assert(!Scene().OwnerModalToken());
        assert(!Scene().IsVisible(Scene().RegionId(runtime::kOwnerTaskPanelRegion)));
        const auto terminal = app.TakeOwnerTaskTerminal();
        assert(terminal && terminal->identity == identity);
        assert(terminal->outcome == runtime::TaskOutcome::Failed);
        assert(terminal->failure &&
               terminal->failure->code == runtime::TaskFailureCode::PreparationFailed);
        assert(!app.TakeOwnerTaskTerminal());
    }

    runtime::InteractionResult Click(const std::shared_ptr<const runtime::InputSnapshot> &input,
                                     std::string_view action)
    {
        const auto found =
            std::find_if(input->nodes.begin(), input->nodes.end(), [action](const auto &node) {
                return node.visible && node.action == action;
            });
        assert(found != input->nodes.end());
        const auto bounds = found->bounds;
        contracts::PointerButtonEvent down;
        down.window = window;
        down.position = {bounds.x + bounds.width / 2, bounds.y + bounds.height / 2};
        down.button = contracts::PointerButton::Primary;
        down.state = contracts::ButtonState::Pressed;
        auto up = down;
        up.state = contracts::ButtonState::Released;
        Scene().HandleInput(down, input);
        return Scene().HandleInput(up, input);
    }
};

void RefreshRejectsOldRows()
{
    Fixture f;
    auto view = View();
    const auto identity = f.Begin(view);
    f.Ready(identity);
    const auto old_input = f.shown;
    const auto old_token = f.Scene().OwnerModalToken();
    view.rows[0] = {"other.txt", false, true};
    view.page_caption = "2 / 2";
    assert(f.app.UpdateOwnerFileTask(identity, view));
    assert(f.app.ActiveOwnerTask()->phase == runtime::TaskPhase::Preparing);
    assert(f.Scene().OwnerModalToken() != old_token);
    assert(!f.Click(old_input, runtime::kOwnerFileRowActions[0]).activation);
    f.Scene().ApplyInputSnapshot(old_input);
    f.app.impl_->AdoptOwnerTaskInput(old_input, f.app.impl_->installed_ui);
    assert(f.app.ActiveOwnerTask()->phase == runtime::TaskPhase::Preparing);
    f.Ready(identity);
    const auto clicked = f.Click(f.shown, runtime::kOwnerFileRowActions[0]);
    assert(clicked.activation && clicked.activation->action == runtime::kOwnerFileRowActions[0]);
    assert(f.app.CancelOwnerTask(identity));
    assert(!f.app.impl_->owner_file_view && !f.app.impl_->owner_file_kind);
    assert(f.app.impl_->owner_task_bindings.empty());
    assert(!f.Scene().IsVisible(f.Scene().RegionId(runtime::kOwnerTaskPanelRegion)));
    assert(f.app.TakeOwnerTaskTerminal()->outcome == runtime::TaskOutcome::Cancelled);
}

void TextEchoAndFullName()
{
    Fixture f;
    auto view = View();
    view.filename.clear();
    view.submit_enabled = false;
    const auto identity = f.Begin(view);
    f.Ready(identity);
    auto projected = std::get<std::string>(
        f.app.impl_->owner_task_bindings.at("__prism_task_file_selected_caption"));
    assert(projected.find('\n') != std::string::npos);
    std::erase(projected, '\n');
    assert(projected == view.selected_caption);
    for (unsigned count = 0;
         count < 32 && f.Scene().FocusedAction() != runtime::kOwnerFileNameAction; ++count) {
        assert(f.Scene().FocusNext());
    }
    assert(f.Scene().FocusedAction() == runtime::kOwnerFileNameAction);
    f.Ready(identity);
    const auto edited = f.Scene().HandleInput(contracts::TextInputEvent{window, "a.txt"}, f.shown);
    assert(edited.text_edit && edited.text_edit->action == runtime::kOwnerFileNameAction);
    view.filename = edited.text_edit->text;
    view.submit_enabled = true;
    const auto token = f.Scene().OwnerModalToken();
    assert(f.app.UpdateOwnerFileTask(identity, view));
    assert(f.Scene().OwnerModalToken() == token);
    assert(f.Scene().FocusedAction() == runtime::kOwnerFileNameAction);
    assert(f.app.ActiveOwnerTask()->phase == runtime::TaskPhase::Ready);
    f.app.RetireOwnerTasks();
    assert(!f.app.impl_->owner_file_view && f.app.impl_->owner_task_bindings.empty());
    assert(!f.app.SupportsOwnerFileTasks());
}

void CapabilityAndConfirmationGuard()
{
    Fixture f;
    assert(!f.app.BeginOwnerConfirmation(Request()));
    f.app.impl_->config.width = 319;
    f.app.impl_->ui_metrics.logical_size.width = 319;
    assert(!f.app.SupportsOwnerFileTasks());
    f.app.impl_->config.width = 640;
    f.app.impl_->ui_metrics.logical_size.width = 640;
    f.app.impl_->config.height = 239;
    f.app.impl_->ui_metrics.logical_size.height = 239;
    assert(!f.app.SupportsOwnerFileTasks());
    auto invalid = View();
    invalid.status.assign(1025, 'x');
    assert(!f.app.BeginOwnerFileTask(Request(), invalid));
}

contracts::NodeId Action(const Fixture &fixture, std::string_view action)
{
    const auto input = fixture.app.impl_->scene->InputGeometry();
    assert(input);
    for (const auto &node : input->nodes) {
        if (node.id && node.visible && node.action == action) {
            return node.id;
        }
    }
    return {};
}

void BusyControlsMatchProvider()
{
    constexpr std::array sizes{contracts::LogicalSize{640, 420}, contracts::LogicalSize{320, 240}};
    for (const auto size : sizes) {
        Fixture f;
        f.app.impl_->config.width = static_cast<int>(size.width);
        f.app.impl_->config.height = static_cast<int>(size.height);
        f.Resize(static_cast<int>(size.width), static_cast<int>(size.height));
        auto view = View();
        const auto identity = f.Begin(view);
        f.Ready(identity);
        assert(f.Scene().State(Action(f, runtime::kOwnerFileNameAction)).enabled);

        view.loading = true;
        view.nav_enabled = false;
        view.rows = {};
        view.selected_caption.clear();
        view.status = "Loading directory";
        assert(f.app.UpdateOwnerFileTask(identity, view));
        f.Ready(identity);
        assert(!f.Scene().State(Action(f, runtime::kOwnerFileNameAction)).enabled);
        assert(!f.Scene().State(Action(f, runtime::kOwnerFileSubmitAction)).enabled);
        f.Click(f.shown, runtime::kOwnerFileNameAction);
        assert(!f.Scene()
                    .HandleInput(contracts::TextInputEvent{window, "ignored"}, f.shown)
                    .text_edit);

        view.status = "Checking selection";
        assert(f.app.UpdateOwnerFileTask(identity, view));
        f.Ready(identity);
        assert(!f.Scene().State(Action(f, runtime::kOwnerFileNameAction)).enabled);

        view.loading = false;
        view.overwrite = true;
        view.status = "Replace the existing file?";
        assert(f.app.UpdateOwnerFileTask(identity, view));
        f.Ready(identity);
        assert(f.Scene().State(Action(f, runtime::kOwnerFileBackAction)).enabled);
        assert(!f.Scene().State(Action(f, runtime::kOwnerFileNameAction)).enabled);

        view.loading = true;
        view.status = "Checking existing file";
        assert(f.app.UpdateOwnerFileTask(identity, view));
        f.Ready(identity);
        assert(!f.Scene().State(Action(f, runtime::kOwnerFileBackAction)).enabled);
        assert(!f.Scene().State(Action(f, runtime::kOwnerFileReplaceAction)).enabled);
        assert(f.Scene().State(Action(f, runtime::kOwnerTaskCancelAction)).enabled);
        assert(!f.Click(f.shown, runtime::kOwnerFileBackAction).activation);
        assert(f.app.CancelOwnerTask(identity));
    }
}

void SupportedResizePreservesOverwrite()
{
    Fixture f;
    auto view = View();
    view.overwrite = true;
    const auto identity = f.Begin(view);
    f.Ready(identity);
    const auto token = f.Scene().OwnerModalToken();
    constexpr std::array sizes{contracts::LogicalSize{320, 240}, contracts::LogicalSize{500, 420},
                               contracts::LogicalSize{640, 420}};
    for (const auto size : sizes) {
        f.Resize(static_cast<int>(size.width), static_cast<int>(size.height));
        assert(f.app.SupportsOwnerFileTasks());
        assert(f.app.ActiveOwnerTask() ==
               (runtime::TaskEntry{identity, runtime::TaskPhase::Ready}));
        assert(f.Scene().OwnerModalToken() == token);
        assert(f.app.impl_->owner_file_view == view);
        assert(!f.app.TakeOwnerTaskTerminal());
        f.Ready(identity);
        assert(f.Scene().State(Action(f, runtime::kOwnerFileBackAction)).enabled);
        assert(f.Scene().State(Action(f, runtime::kOwnerFileReplaceAction)).enabled);
        auto projected = std::get<std::string>(
            f.app.impl_->owner_task_bindings.at("__prism_task_file_selected_caption"));
        std::erase(projected, '\n');
        assert(projected == view.selected_caption);
    }
    assert(f.app.CancelOwnerTask(identity));
}

void UndersizedResizeFailsOnce()
{
    constexpr std::array sizes{contracts::LogicalSize{244, 204}, contracts::LogicalSize{319, 420},
                               contracts::LogicalSize{640, 239}};
    for (const auto size : sizes) {
        Fixture f;
        auto view = View();
        view.overwrite = true;
        const auto identity = f.Begin(view);
        f.Ready(identity);
        const auto old_input = f.shown;
        f.Resize(static_cast<int>(size.width), static_cast<int>(size.height));
        assert(!f.app.SupportsOwnerFileTasks());
        f.CheckFailedAndHidden(identity);
        assert(!f.Click(old_input, runtime::kOwnerFileReplaceAction).activation);
        f.Resize(640, 420);
        assert(f.app.SupportsOwnerFileTasks());
        assert(!f.app.ActiveOwnerTask() && !f.app.TakeOwnerTaskTerminal());
    }
}

void UnreadableRootRejectsPreparation()
{
    for (const bool rounded_clip : {false, true}) {
        Fixture f;
        const auto root_id = f.Scene().InputGeometry()->root;
        if (rounded_clip) {
            assert(f.Scene().SetProperty(root_id, runtime::DslProperty::Clip, true));
            assert(f.Scene().SetProperty(root_id, runtime::DslProperty::Radius, 130.0));
        } else {
            assert(f.Scene().SetProperty(root_id, runtime::DslProperty::Padding, 32.0));
        }
        f.Resize(320, 420);
        assert(f.app.SupportsOwnerFileTasks());
        auto view = View();
        view.overwrite = true;
        bool rejected = false;
        try {
            f.app.BeginOwnerFileTask(Request(), view);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        assert(rejected);
        assert(!f.app.ActiveOwnerTask() && !f.app.impl_->owner_task_scope);
        assert(!f.app.impl_->owner_file_view && f.app.impl_->owner_task_bindings.empty());
        assert(!f.Scene().OwnerModalToken());
        assert(!f.Scene().IsVisible(f.Scene().RegionId(runtime::kOwnerTaskPanelRegion)));
        assert(!f.app.TakeOwnerTaskTerminal());
    }
}

void ActiveRootGeometryFailsOnce()
{
    Fixture f;
    auto view = View();
    view.overwrite = true;
    const auto identity = f.Begin(view);
    f.Ready(identity);
    assert(f.Scene().SetProperty(f.Scene().InputGeometry()->root, runtime::DslProperty::Padding,
                                 32.0));
    f.Resize(320, 420);
    assert(f.app.SupportsOwnerFileTasks());
    f.CheckFailedAndHidden(identity);
}

void ConcaveClipRejectsWholeButton()
{
    // This notch crosses the Cancel body without excluding any of its four corners.
    const contracts::Contour contour{
        {{0, 0}, {640, 0}, {640, 420}, {0, 420}, {0, 378}, {80, 378}, {80, 370}, {0, 370}}};
    Fixture f{contour};
    auto view = View();
    view.overwrite = true;
    bool rejected = false;
    try {
        f.app.BeginOwnerFileTask(Request(), view);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
    assert(!f.app.ActiveOwnerTask() && !f.app.impl_->owner_task_scope);
    assert(!f.app.impl_->owner_file_view && f.app.impl_->owner_task_bindings.empty());
    assert(!f.Scene().OwnerModalToken());
    assert(!f.Scene().IsVisible(f.Scene().RegionId(runtime::kOwnerTaskPanelRegion)));
    assert(!f.app.TakeOwnerTaskTerminal());
}
} // namespace

int main()
{
    RefreshRejectsOldRows();
    TextEchoAndFullName();
    CapabilityAndConfirmationGuard();
    BusyControlsMatchProvider();
    SupportedResizePreservesOverwrite();
    UndersizedResizeFailsOnce();
    UnreadableRootRejectsPreparation();
    ActiveRootGeometryFailsOnce();
    ConcaveClipRejectsWholeButton();
    std::cout << "client_owner_file_panel_test: passed\n";
}
