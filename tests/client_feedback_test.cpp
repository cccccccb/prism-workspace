#include "client_application_p.hpp"
#include "prism/contracts/display_list_validation.hpp"
#include "prism/runtime/owner_task_panel.hpp"
#include "prism/theme/compiler.hpp"
#include "prism/theme/motion_compiler.hpp"
#include "scene_p.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

using namespace prism;

namespace {
const std::filesystem::path source_root{PRISM_SOURCE_ROOT};
constexpr contracts::InputSource pointer{0, 1, 1};
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

std::string Read(const std::filesystem::path &path)
{
    std::ifstream file(path);
    assert(file);
    return {std::istreambuf_iterator<char>(file), {}};
}

sdk::ClientConfig Config()
{
    sdk::ClientConfig config;
    config.app_id = "feedback-unit-fixture";
    config.font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    config.width = 640;
    config.height = 420;
    config.install_limits.nodes_per_turn = 64;
    config.install_limits.cpu_per_turn = std::chrono::milliseconds(10);
    return config;
}

runtime::PreparedComponent Master()
{
    return runtime::PrepareComponent(R"(
Card(material:"window",padding:"@window_padding") {
    VStack(spacing:8) {
        TextField($document,action:"edit",height:80,font:"@font_body")
        Button("Owner",action:$owner_action,height:32)
        Button("Menu",action:"menu-owner",height:32)
        Card(flex:1)
        Card(height:32) { Text("Later") }
    }
    Menu("menu-owner",width:180,height:100) { MenuItem(action:"menu-item",height:32) { Text("Item") } }
})");
}

contracts::OwnerFeedbackRequest Request(std::uint64_t id = 19)
{
    return {id,
            contracts::OwnerFeedbackKind::Success,
            "Saved",
            "Changes saved.\nContinue editing.",
            {{1, "Undo"}, {2, "View"}},
            0};
}

const runtime::InputSnapshotNode &Action(const runtime::InputSnapshot &input,
                                         std::string_view action)
{
    for (const auto &node : input.nodes) {
        if (node.visible && node.action == action) {
            return node;
        }
    }
    assert(false && "Visible action missing");
    return input.nodes.front();
}

// Non-native unit fixture: real DSL, shaping, SDK transactions and immutable
// frames. Metadata adoption is simulated, without Wayland, EGL or GPU rendering.
struct Fixture {
    sdk::ClientApplication app{Config()};
    std::shared_ptr<const runtime::FramePacket> shown;

    Fixture()
    {
        assert(app.ApplyTheme(InstantTheme(source_root / "resources/themes", "glass", 1)));
        assert(app.ConfigureOwnerFeedbackPanel(runtime::PrepareComponent(
            Read(source_root / "resources/ui/owner-feedback-panel.prism"))));
        assert(app.ConfigureOwnerTaskPanel(
            runtime::PrepareComponent(Read(source_root / "resources/ui/owner-task-panel.prism"))));
        auto &impl = *app.impl_;
        impl.opened_once = true;
        impl.ui_configure_count = 1;
        impl.ui_metrics = {{640, 420}, {640, 420}, 1};
        impl.binding_values = {{"document", std::string("Unsaved draft")},
                               {"owner_action", std::string("owner")}};
        runtime::LoadDiagnostic diagnostic;
        const auto installed = impl.InstallScene(app.BeginUiLoad(), Master(), &diagnostic);
        if (!installed) {
            std::cerr << diagnostic.message << '\n';
        }
        assert(installed && diagnostic.message.empty());
        impl.PublishFramePacket();
        Submit(impl.queued_frame);
        assert(app.SupportsOwnerFeedback());
        assert(!Scene().IsVisible(Scene().RegionId(runtime::kOwnerFeedbackPanelRegion)));
    }

    runtime::Scene &Scene()
    {
        return *app.impl_->scene;
    }

    void Submit(const std::shared_ptr<const runtime::FramePacket> &frame, bool prepared = true)
    {
        assert(frame && frame->input_snapshot && frame->display_list);
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

    std::string FeedbackAction(std::uint32_t id)
    {
        return runtime::OwnerFeedbackActionName(app.impl_->owner_feedback_generation,
                                                app.impl_->owner_feedback->request_id, id);
    }

    void Click(std::string_view action)
    {
        const auto bounds = Action(*shown->input_snapshot, action).bounds;
        const contracts::LogicalPoint point{bounds.x + bounds.width / 2,
                                            bounds.y + bounds.height / 2};
        contracts::PointerButtonEvent down{window,
                                           point,
                                           contracts::PointerButton::Primary,
                                           contracts::ButtonState::Pressed,
                                           0,
                                           1,
                                           pointer,
                                           79};
        auto up = down;
        up.state = contracts::ButtonState::Released;
        app.impl_->HandleWindowEvent(down, shown->input_snapshot);
        app.impl_->HandleWindowEvent(up, shown->input_snapshot);
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
};

void CheckAdoptionAndNoFocusGrab()
{
    Fixture fixture;
    auto &app = fixture.app;
    auto &impl = *app.impl_;
    fixture.Scene().FocusNext();
    assert(fixture.Scene().FocusedAction() == "edit");
    const auto epoch = fixture.Scene().OwnerModalEpoch();
    assert(app.ShowOwnerFeedback(Request()));
    assert(fixture.Scene().FocusedAction() == "edit");
    assert(fixture.Scene().OwnerModalEpoch() == epoch);
    assert(!impl.owner_feedback_session.Adopted() && !impl.owner_feedback_session.Deadline());
    fixture.Submit(impl.queued_frame, false);
    assert(!impl.owner_feedback_session.Adopted());
    fixture.Submit(impl.queued_frame);
    assert(impl.owner_feedback_session.Adopted() && impl.owner_feedback_session.Deadline());
    assert(app.SetBinding("owner_action", fixture.FeedbackAction(1)));
    impl.PublishFramePacket();
    fixture.Submit(impl.queued_frame);
    const auto ordinary = Action(*fixture.shown->input_snapshot, fixture.FeedbackAction(1));
    // The ordinary button precedes the trusted panel; its bound reserved string
    // must never be accepted as a Host feedback activation.
    assert(!fixture.Scene().IsNodeInRegion(ordinary.id, runtime::kOwnerFeedbackPanelRegion));
    runtime::InteractionResult injected;
    injected.activation = runtime::Activation{ordinary.id, fixture.FeedbackAction(1)};
    impl.CompleteInteractionResult(injected, impl.installed_ui);
    assert(impl.owner_feedback && !app.TakeOwnerFeedbackAction());
    assert(app.SetBinding("owner_action", std::string("owner")));
    impl.PublishFramePacket();
    fixture.Submit(impl.queued_frame);
    assert(impl.FeedbackTimeoutMs(-1) > 0 && impl.FeedbackTimeoutMs(-1) <= 4000);
    const auto bounds =
        fixture.Scene().Bounds(fixture.Scene().RegionId(runtime::kOwnerFeedbackCardRegion));
    impl.HandleWindowEvent(
        contracts::PointerMotionEvent{window, {bounds.x + 10, bounds.y + 10}, 2, pointer},
        fixture.shown->input_snapshot);
    assert(!impl.owner_feedback_session.Deadline());
    impl.HandleWindowEvent(contracts::PointerLeaveEvent{window, 3, pointer},
                           fixture.shown->input_snapshot);
    assert(impl.owner_feedback_session.Deadline());
    const contracts::LogicalPoint old_position{bounds.x + 10, bounds.y + 10};
    impl.HandleWindowEvent(
        contracts::PointerButtonEvent{window, old_position, contracts::PointerButton::Primary,
                                      contracts::ButtonState::Released, 0, 4, pointer, 0},
        fixture.shown->input_snapshot);
    impl.HandleWindowEvent(contracts::PointerScrollEvent{window, old_position, 0, 0, 5, pointer},
                           fixture.shown->input_snapshot);
    assert(impl.owner_feedback_session.Deadline() && impl.owner_feedback_pointers.empty());
    impl.HandleWindowEvent(contracts::FocusEvent{window, false, pointer, false},
                           fixture.shown->input_snapshot);
    assert(!impl.owner_feedback_session.Deadline());
    impl.HandleWindowEvent(contracts::FocusEvent{window, true, pointer, false},
                           fixture.shown->input_snapshot);
    assert(impl.owner_feedback_session.Deadline());
    fixture.Click(fixture.FeedbackAction(1));
    assert(!impl.owner_feedback && !impl.owner_feedback_session.Deadline());
    assert(app.TakeOwnerFeedbackAction() == (contracts::OwnerFeedbackAction{19, 1}));
    assert(!app.TakeOwnerFeedbackAction());
    assert(fixture.Scene().OwnerModalEpoch() == epoch);
}

void CheckReplacementAndBounds()
{
    Fixture fixture;
    auto &impl = *fixture.app.impl_;
    assert(fixture.app.ShowOwnerFeedback(Request()));
    fixture.Submit(impl.queued_frame);
    const auto old = fixture.shown;
    const auto old_action = fixture.FeedbackAction(1);
    const auto bounds = Action(*old->input_snapshot, old_action).bounds;
    const contracts::LogicalPoint point{bounds.x + bounds.width / 2, bounds.y + bounds.height / 2};
    contracts::PointerButtonEvent down{
        window,  point, contracts::PointerButton::Primary, contracts::ButtonState::Pressed, 0, 1,
        pointer, 79};
    impl.HandleWindowEvent(down, old->input_snapshot);
    assert(fixture.app.ShowOwnerFeedback(Request(20)));
    fixture.Submit(impl.queued_frame);
    auto up = down;
    up.state = contracts::ButtonState::Released;
    impl.HandleWindowEvent(up, old->input_snapshot);
    assert(!fixture.app.TakeOwnerFeedbackAction());
    assert(impl.owner_feedback->request_id == 20);
    assert(!impl.HandleOwnerFeedbackAction({{}, "owner"}));
    assert(
        impl.HandleOwnerFeedbackAction({Action(*old->input_snapshot, old_action).id, old_action}));
    assert(impl.owner_feedback->request_id == 20);
    const auto count = impl.scene->nodes_.size();
    for (std::uint64_t id = 21; id < 1021; ++id) {
        assert(fixture.app.ShowOwnerFeedback(Request(id)));
        // The fixture drains the queue; a native render worker owns this in production.
        while (impl.bridge->render_commands.TryPop()) {
        }
        assert(impl.scene->nodes_.size() == count);
    }
    const auto retired = impl.scene->RegionId(runtime::kOwnerFeedbackCardRegion);
    impl.scene->node_generations_[retired.index] = UINT32_MAX;
    impl.scene->nodes_[retired.index]->id.generation = UINT32_MAX;
    const contracts::NodeId exhausted{retired.index, UINT32_MAX};
    assert(fixture.app.ShowOwnerFeedback(Request(1021)));
    assert(!impl.scene->Find(exhausted));
    assert(!impl.scene->nodes_[retired.index]);
    assert(impl.scene->node_generations_[retired.index] == UINT32_MAX);
    assert(impl.scene->nodes_.size() == count + 1);
    while (impl.bridge->render_commands.TryPop()) {
    }
    assert(!impl.scene->MountRegions(
        std::array{runtime::RegionUpdate{std::string(runtime::kOwnerFeedbackPanelRegion),
                                         runtime::ParseBlueprint("Card {}")}},
        impl.owner_feedback_bindings));
    auto invalid = Request(2000);
    invalid.actions[0].label = std::string(48, 'W');
    const auto previous = impl.owner_feedback;
    const auto previous_id = impl.scene->RegionId(runtime::kOwnerFeedbackCardRegion);
    assert(!fixture.app.ShowOwnerFeedback(invalid));
    assert(impl.owner_feedback == previous &&
           impl.scene->RegionId(runtime::kOwnerFeedbackCardRegion) == previous_id);
    assert(!fixture.app.DismissOwnerFeedback(1));
    assert(fixture.app.DismissOwnerFeedback(previous->request_id));
    assert(!fixture.app.TakeOwnerFeedbackAction());
}

void CheckModalAndLifecycle()
{
    Fixture fixture;
    auto &impl = *fixture.app.impl_;
    assert(fixture.app.ShowOwnerFeedback(Request()));
    fixture.Submit(impl.queued_frame);
    contracts::OwnerTaskRequest confirmation;
    confirmation.request_id = 89;
    confirmation.title = "Keep changes?";
    confirmation.message = "Unsaved work remains.";
    confirmation.choices = {{1, "Keep", contracts::OwnerTaskChoiceRole::Primary}};
    const auto task = fixture.app.BeginOwnerConfirmation(confirmation);
    assert(task && impl.owner_feedback && impl.owner_feedback_hidden);
    assert(!impl.owner_feedback_session.Deadline());
    fixture.Submit(impl.queued_frame);
    assert(fixture.app.CancelOwnerTask(*task));
    assert(fixture.app.TakeOwnerTaskTerminal());
    assert(impl.owner_feedback && !impl.owner_feedback_hidden &&
           !impl.owner_feedback_session.Deadline());
    fixture.Submit(impl.queued_frame);
    assert(impl.owner_feedback_session.Deadline());
    auto error = Request(20);
    error.kind = contracts::OwnerFeedbackKind::Error;
    assert(fixture.app.ShowOwnerFeedback(error));
    fixture.Submit(impl.queued_frame);
    assert(impl.owner_feedback_session.Adopted() && !impl.owner_feedback_session.Deadline());
    fixture.Click(fixture.FeedbackAction(0));
    assert(!impl.owner_feedback && !fixture.app.TakeOwnerFeedbackAction());
    assert(fixture.app.ShowOwnerFeedback(Request(21)));
    assert(impl.InstallScene(fixture.app.BeginUiLoad(), Master(), nullptr));
    assert(!impl.owner_feedback && !fixture.app.TakeOwnerFeedbackAction());
    fixture.app.RetireOwnerFeedback();
    assert(!fixture.app.ShowOwnerFeedback(Request(22)) && !fixture.app.SupportsOwnerFeedback());
}

void CheckTextResizeAndTheme()
{
    Fixture fixture;
    auto &impl = *fixture.app.impl_;
    auto request = Request();
    request.title = "Saved document with a long title for this compact feedback";
    request.message = "This document contains several lines of content.\n";
    while (request.message.size() < 1800) {
        request.message += "More content with details. ";
    }
    assert(fixture.app.ShowOwnerFeedback(request));
    fixture.Submit(impl.queued_frame);
    const auto request_id = impl.owner_feedback->request_id;
    fixture.Resize(240, 180);
    assert(impl.owner_feedback && impl.owner_feedback->request_id == request_id);
    for (const auto name : {"glass", "translucent", "transparent", "square"}) {
        for (const auto scheme : {"light", "dark"}) {
            assert(fixture.app.ApplyTheme(InstantTheme(source_root / "resources/themes", name,
                                                       fixture.app.ThemeGeneration() + 1, scheme)));
            assert(impl.owner_feedback && impl.owner_feedback->request_id == request_id);
            const auto metrics =
                fixture.Scene().TextLayoutInRegion(runtime::kOwnerFeedbackMessageRegion);
            assert(metrics);
            const auto &wrapped =
                std::get<std::string>(impl.owner_feedback_bindings.at("__prism_feedback_message"));
            std::size_t start = 0;
            while (start < wrapped.size()) {
                const auto end = wrapped.find('\n', start);
                const auto line = std::string_view(wrapped).substr(
                    start, end == std::string::npos ? wrapped.size() - start : end - start);
                assert(impl.ShapeText(line, metrics->font_size).width <= metrics->width + 0.001);
                if (end == std::string::npos) {
                    break;
                }
                start = end + 1;
            }
        }
    }
    fixture.Resize(200, 140);
    assert(!impl.owner_feedback && !impl.failed);
}

void CheckExpiryAndFocusPause()
{
    Fixture fixture;
    auto &impl = *fixture.app.impl_;
    assert(fixture.app.ShowOwnerFeedback(Request()));
    fixture.Submit(impl.queued_frame);
    contracts::KeyEvent tab{window, 0x2b, contracts::ButtonState::Pressed, false, 1, {0, 2, 1}};
    auto released = tab;
    released.state = contracts::ButtonState::Released;
    // Real submitted input routing moves keyboard focus into the feedback.
    for (unsigned count = 0;
         count < 8 && !fixture.Scene().HasFocusInRegion(runtime::kOwnerFeedbackPanelRegion);
         ++count) {
        impl.HandleWindowEvent(tab, fixture.shown->input_snapshot);
        impl.HandleWindowEvent(released, fixture.shown->input_snapshot);
    }
    assert(fixture.Scene().HasFocusInRegion(runtime::kOwnerFeedbackPanelRegion));
    assert(!impl.owner_feedback_session.Deadline());
    for (unsigned count = 0; count < 8 && fixture.Scene().FocusedAction() != "edit"; ++count) {
        impl.HandleWindowEvent(tab, fixture.shown->input_snapshot);
        impl.HandleWindowEvent(released, fixture.shown->input_snapshot);
    }
    assert(fixture.Scene().FocusedAction() == "edit");
    assert(impl.owner_feedback_session.Deadline());

    // Simulate an elapsed deadline only in this unit fixture, with no sleeping
    // or production clock hook. Reconcile is the same path used by SDK Pump.
    const auto now = fixture.Scene().AnimationNowNs();
    impl.owner_feedback_session.Begin(impl.owner_feedback_generation, 1000);
    assert(impl.owner_feedback_session.Adopt(impl.owner_feedback_generation, now - 1000, false));
    assert(impl.FeedbackTimeoutMs(-1) == 0);
    impl.ReconcileOwnerFeedback();
    assert(!impl.owner_feedback && !impl.owner_feedback_session.Deadline());
    assert(
        !fixture.Scene().IsVisible(fixture.Scene().RegionId(runtime::kOwnerFeedbackPanelRegion)));
    assert(fixture.Scene().FocusedAction() == "edit");
    assert(!fixture.app.TakeOwnerFeedbackAction());
}

void CheckReservedAndPostCommitFailure()
{
    Fixture fixture;
    assert(!fixture.app.SetBinding("__prism_feedback_title", std::string("Caller")));
    auto &impl = *fixture.app.impl_;
    const auto previous = impl.scene.get();
    assert(!impl.InstallScene(
        fixture.app.BeginUiLoad(),
        runtime::PrepareComponent("Card { Button(\"Bad\",action:\"__prism_feedback_dismiss\") }"),
        nullptr));
    assert(impl.scene.get() == previous);
    assert(fixture.app.ShowOwnerFeedback(Request()));
    fixture.Submit(impl.queued_frame);
    impl.bridge->render_commands.TryPop();
    while (impl.bridge->render_commands.TryPush(runtime::RenderCommand(
               runtime::RequestRenderCommand{})) == runtime::QueuePushResult::Accepted) {
    }
    assert(!fixture.app.ShowOwnerFeedback(Request(20)));
    assert(impl.failed && !impl.owner_feedback && !fixture.app.SupportsOwnerFeedback());
}
} // namespace

int main()
{
    CheckAdoptionAndNoFocusGrab();
    CheckReplacementAndBounds();
    CheckModalAndLifecycle();
    CheckTextResizeAndTheme();
    CheckExpiryAndFocusPause();
    CheckReservedAndPostCommitFailure();
}
