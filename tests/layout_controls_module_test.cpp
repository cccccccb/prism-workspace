#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/sdk/module_session.hpp"
#include "prism/theme/compiler.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace prism;

namespace {
struct Fixture {
    std::map<std::string, runtime::PropertyValue, std::less<>> bindings;
    std::vector<contracts::LayoutControlRequest> requests;
    std::size_t subscriptions{};

    bool Binding(std::string_view key, runtime::PropertyValue value)
    {
        bindings.insert_or_assign(std::string(key), std::move(value));
        return true;
    }

    std::uint64_t Subscribe(bool enabled)
    {
        assert(enabled);
        ++subscriptions;
        return 91;
    }

    bool Send(const contracts::LayoutControlRequest &request)
    {
        requests.push_back(request);
        return true;
    }

    bool Visible() const
    {
        return std::get<bool>(bindings.at("handle_visible"));
    }

    double Number(std::string_view key) const
    {
        return std::get<double>(bindings.at(std::string(key)));
    }

    static int32_t AbiBinding(void *context, PrismStringViewV1 key, PrismValueV1 value)
    {
        auto &fixture = *static_cast<Fixture *>(context);
        if (value.kind == PRISM_VALUE_BOOL_V1) {
            fixture.Binding({key.data, key.size}, value.as.boolean != 0);
        } else if (value.kind == PRISM_VALUE_NUMBER_V1) {
            fixture.Binding({key.data, key.size}, value.as.number);
        } else {
            fixture.Binding({key.data, key.size},
                            std::string(value.as.string.data, value.as.string.size));
        }
        return 0;
    }

    static int32_t AbiReady(void *)
    {
        return 0;
    }

    static int32_t AbiTick(void *, std::uint64_t)
    {
        assert(false); // This event-driven module must never schedule a timer.
        return -1;
    }

    static std::uint64_t AbiSubscribe(void *context, std::uint32_t enabled)
    {
        return static_cast<Fixture *>(context)->Subscribe(enabled != 0);
    }

    static int32_t AbiControl(void *, const PrismLayoutCommandV1 *)
    {
        assert(false);
        return -1;
    }
};

contracts::LayoutStateEvent Layout()
{
    contracts::LayoutStateEvent event;
    event.subscription = 91;
    auto &state = event.snapshot;
    state.session = 10;
    state.revision = 1;
    state.topology_revision = 2;
    state.layout_revision = 3;
    state.focus_revision = 4;
    state.outputs.push_back({20, "headless", {0, 0, 1024, 600}, 1, true, true});
    state.workspaces.push_back({30, 40, 20, "workspace", true});
    state.boundaries.push_back(
        {50, 40, 41, 42, 30, contracts::LayoutBoundaryAxis::X, {508, 52, 8, 500}, true, true});
    state.control_handle = {50, {488, 278, 48, 48}, true};
    return event;
}

contracts::GestureEvent Begin(std::uint64_t id)
{
    contracts::GestureEvent event;
    event.id = id;
    event.node = {3, 1};
    event.action = "boundary:resize";
    event.source = {1, 2, 3};
    event.serial = 88;
    event.start = {24, 24};
    event.position = {31, 24};
    event.snapshot_scene = 50;
    event.snapshot_version = 60;
    return event;
}

contracts::LayoutControlResult Result(const contracts::LayoutControlRequest &request,
                                      contracts::LayoutControlStatus status)
{
    contracts::LayoutControlResult result;
    result.request = request.request;
    result.gesture = request.gesture;
    result.session = request.session ? request.session : 1000 + request.gesture;
    result.sequence = request.sequence;
    result.status = status;
    return result;
}

void ConfirmLatest(sdk::ModuleSession &module, const Fixture &fixture,
                   contracts::LayoutControlStatus status)
{
    module.Deliver(Result(fixture.requests.back(), status));
}

void CheckContinuousResize(sdk::ModuleSession &module, Fixture &fixture,
                           contracts::LayoutStateEvent &layout)
{
    auto gesture = Begin(4);
    module.Gesture(gesture);
    assert(fixture.requests.size() == 1);
    const auto begin = fixture.requests.back();
    assert(begin.phase == contracts::LayoutControlPhase::Begin);
    assert(begin.operation == contracts::LayoutControlOperation::BoundaryGesture);
    assert(begin.target.boundary == 50 && begin.target.workspace == 30);
    assert(begin.target.root == 40 && begin.target.output == 20);
    assert(begin.target.wm_session == 10 && begin.input.serial == 88);
    ConfirmLatest(module, fixture, contracts::LayoutControlStatus::Began);

    gesture.phase = contracts::GesturePhase::Update;
    gesture.position.x += 60;
    module.Gesture(gesture);
    assert(fixture.requests.size() == 2);
    assert(fixture.requests.back().phase == contracts::LayoutControlPhase::Update);
    ConfirmLatest(module, fixture, contracts::LayoutControlStatus::Updated);

    // A live resize and subsequent committed buffer can publish new geometry.
    // They must not cancel the module's own gesture or replace its Begin target.
    ++layout.snapshot.revision;
    ++layout.snapshot.layout_revision;
    layout.snapshot.boundaries.front().bounds.x += 60;
    module.Deliver(layout);
    ++layout.snapshot.revision;
    ++layout.snapshot.focus_revision;
    module.Deliver(layout);
    assert(fixture.requests.size() == 2);

    gesture.phase = contracts::GesturePhase::End;
    module.Gesture(gesture);
    assert(fixture.requests.size() == 3);
    const auto end = fixture.requests.back();
    assert(end.phase == contracts::LayoutControlPhase::End);
    assert(end.intent == contracts::LayoutControlIntent::ApplyBoundary);
    assert(end.target == begin.target);
    ConfirmLatest(module, fixture, contracts::LayoutControlStatus::Ended);
}

void CheckInvalidation(sdk::ModuleSession &module, Fixture &fixture,
                       contracts::LayoutStateEvent &layout)
{
    auto gesture = Begin(5);
    module.Gesture(gesture);
    ConfirmLatest(module, fixture, contracts::LayoutControlStatus::Began);
    const auto before = fixture.requests.size();
    ++layout.snapshot.revision;
    ++layout.snapshot.topology_revision;
    module.Deliver(layout);
    assert(fixture.requests.size() == before + 1);
    assert(fixture.requests.back().phase == contracts::LayoutControlPhase::Cancel);
    ConfirmLatest(module, fixture, contracts::LayoutControlStatus::Cancelled);
    gesture.phase = contracts::GesturePhase::End;
    module.Gesture(gesture);
    assert(fixture.requests.size() == before + 1);

    module.Gesture(Begin(6));
    ConfirmLatest(module, fixture, contracts::LayoutControlStatus::Began);
    ++layout.snapshot.revision;
    layout.snapshot.control_handle = {};
    module.Deliver(layout);
    assert(!fixture.Visible());
    assert(fixture.requests.back().phase == contracts::LayoutControlPhase::Cancel);
    ConfirmLatest(module, fixture, contracts::LayoutControlStatus::Cancelled);
    const auto hidden_count = fixture.requests.size();
    module.Gesture(Begin(7));
    assert(fixture.requests.size() == hidden_count);

    ++layout.snapshot.revision;
    layout.snapshot.control_handle = {50, {548, 278, 48, 48}, true};
    layout.snapshot.boundaries.front().axis = contracts::LayoutBoundaryAxis::Y;
    module.Deliver(layout);
    assert(fixture.Visible() && fixture.Number("handle_width") == 32 &&
           fixture.Number("handle_height") == 4);
    module.Deliver(Layout());
    assert(fixture.Number("handle_width") == 32);

    module.Gesture(Begin(8));
    ConfirmLatest(module, fixture, contracts::LayoutControlStatus::Rejected);
    gesture = Begin(8);
    gesture.phase = contracts::GesturePhase::End;
    const auto rejected_count = fixture.requests.size();
    module.Gesture(gesture);
    assert(fixture.requests.size() == rejected_count);
}

void CheckPendingEnd(sdk::ModuleSession &module, Fixture &fixture)
{
    auto gesture = Begin(9);
    module.Gesture(gesture);
    const auto before = fixture.requests.size();
    const auto begin = fixture.requests.back();
    gesture.phase = contracts::GesturePhase::End;
    gesture.position.x += 80;
    module.Gesture(gesture);
    module.Gesture(Begin(10));
    assert(fixture.requests.size() == before);

    module.Deliver(Result(begin, contracts::LayoutControlStatus::Began));
    assert(fixture.requests.size() == before + 1);
    assert(fixture.requests.back().phase == contracts::LayoutControlPhase::End);
    assert(fixture.requests.back().intent == contracts::LayoutControlIntent::ApplyBoundary);
    ConfirmLatest(module, fixture, contracts::LayoutControlStatus::Ended);
}

void CheckWindowModule(sdk::ModuleSession &module, Fixture &fixture)
{
    auto layout = Layout();
    layout.snapshot.revision = 100;
    layout.snapshot.control_handle = {0, {100, 100, 168, 56}, true, 41};
    contracts::LayoutNode root;
    root.id = 40;
    root.workspace = 30;
    root.children = {41, 42};
    layout.snapshot.nodes.push_back(root);
    contracts::LayoutNode view;
    view.id = 41;
    view.parent = 40;
    view.workspace = 30;
    view.kind = contracts::LayoutNodeKind::View;
    view.visible = true;
    layout.snapshot.nodes.push_back(view);
    module.Deliver(layout);
    assert(!fixture.Visible() && std::get<bool>(fixture.bindings.at("window_visible")));
    assert(std::get<bool>(fixture.bindings.at("split_enabled")));

    for (int slot = 0; slot < 3; ++slot) {
        auto gesture = Begin(100 + slot);
        gesture.action = slot == 0   ? "window:fullscreen"
                         : slot == 1 ? "window:horizontal"
                                     : "window:vertical";
        gesture.start = gesture.position = {32.0 + 52 * slot, 28};
        const auto count = fixture.requests.size();
        module.Gesture(gesture);
        assert(fixture.requests.size() == count + 1);
        assert(fixture.requests.back().target.node == 41 &&
               !fixture.requests.back().target.boundary);
        assert(fixture.requests.back().operation ==
               contracts::LayoutControlOperation::WindowGesture);
        gesture.phase = contracts::GesturePhase::End;
        module.Gesture(gesture); // Release before Begin acknowledgement must remain queued.
        assert(fixture.requests.size() == count + 1);
        ConfirmLatest(module, fixture, contracts::LayoutControlStatus::Began);
        assert(fixture.requests.size() == count + 2);
        const auto expected = slot == 0   ? contracts::LayoutControlIntent::EnterWindowFullscreen
                              : slot == 1 ? contracts::LayoutControlIntent::SplitHorizontal
                                          : contracts::LayoutControlIntent::SplitVertical;
        assert(fixture.requests.back().intent == expected);
        ConfirmLatest(module, fixture, contracts::LayoutControlStatus::Ended);
    }
    layout.snapshot.revision++;
    layout.snapshot.nodes[1].fullscreen = true;
    module.Deliver(layout);
    assert(!std::get<bool>(fixture.bindings.at("split_enabled")));
    assert(std::get<std::string>(fixture.bindings.at("fullscreen_icon")) == "restore");
    auto gesture = Begin(104);
    gesture.action = "window:fullscreen";
    gesture.start = gesture.position = {32, 28};
    module.Gesture(gesture);
    ConfirmLatest(module, fixture, contracts::LayoutControlStatus::Began);
    gesture.position = {84,
                        28}; // Ending on a different button cancels rather than switching intent.
    gesture.phase = contracts::GesturePhase::End;
    module.Gesture(gesture);
    assert(fixture.requests.back().phase == contracts::LayoutControlPhase::Cancel);
    ConfirmLatest(module, fixture, contracts::LayoutControlStatus::Cancelled);
}

runtime::ShapedText Shape(std::string_view text, double font)
{
    return {{}, text.size() * font * 0.5, font};
}

void CheckUi()
{
    const auto path =
        std::filesystem::path(PRISM_SOURCE_ROOT) / "prism-layout-controls/ui/handle.prism";
    std::ifstream source(path);
    assert(source);
    const std::string text{std::istreambuf_iterator<char>{source}, {}};
    const auto blueprint = runtime::ParseBlueprint(text);
    assert(blueprint.children.size() == 2);
    assert(blueprint.children.front().kind == runtime::Kind::InteractionTarget);
    assert(blueprint.children.front().children.size() == 1);

    for (const auto *theme_id : {"glass", "translucent", "transparent", "square"}) {
        for (const auto *scheme : {"light", "dark"}) {
            const auto theme = theme::LoadTheme(theme::DefaultThemeRoot(), theme_id, 1, scheme);
            runtime::Scene scene(blueprint, Shape, {}, theme);
            scene.SetBinding("handle_visible", true);
            scene.SetBinding("window_visible", false);
            scene.SetBinding("split_enabled", false);
            scene.SetBinding("split_opacity", 1.0);
            scene.SetBinding("fullscreen_icon", std::string("fullscreen"));
            scene.SetBinding("handle_width", 4.0);
            scene.SetBinding("handle_height", 32.0);
            assert(scene.SetViewport({48, 48}));
            assert(scene.Build(contracts::WindowId{1}));
            const auto target = scene.HitTest({24, 24});
            assert(target);
            const auto target_bounds = scene.Bounds(target->node);
            assert(target_bounds.width == 48 && target_bounds.height == 48);

            scene.SetBinding("handle_width", 32.0);
            scene.SetBinding("handle_height", 4.0);
            assert(scene.Build(contracts::WindowId{1}));
            assert(scene.HitTest({24, 24})->node == target->node);
            assert(scene.Bounds(target->node) == target_bounds);
            assert(!scene.HasActiveAnimations());

            scene.SetBinding("handle_visible", false);
            assert(scene.Build(contracts::WindowId{1}));
            assert(!scene.HitTest({24, 24}));
            assert(scene.InputRegions().empty());

            scene.SetBinding("window_visible", true);
            scene.SetBinding("split_enabled", true);
            assert(scene.SetViewport({168, 56}));
            assert(scene.Build(contracts::WindowId{1}));
            for (int slot = 0; slot < 3; ++slot) {
                const auto hit = scene.HitTest({32.0 + slot * 52, 28});
                assert(hit);
                const auto bounds = scene.Bounds(hit->node);
                assert(bounds.x == 8 + slot * 52 && bounds.y == 8);
                assert(bounds.width == 48 && bounds.height == 40);
            }
            const auto horizontal = scene.HitTest({84, 28})->node;
            const auto vertical = scene.HitTest({136, 28})->node;
            scene.SetBinding("split_enabled", false);
            scene.SetBinding("split_opacity", 1.0);
            assert(scene.Build(contracts::WindowId{1}));
            assert(scene.HitTest({32, 28}));
            // Disabled buttons fall back to the palette shield, never the app below.
            assert(scene.HitTest({84, 28}) && scene.HitTest({84, 28})->node != horizontal);
            assert(scene.HitTest({136, 28}) && scene.HitTest({136, 28})->node != vertical);
        }
    }
}

void CheckOptionalAbiTail(const std::filesystem::path &module_path)
{
    Fixture fixture;
    PrismHostApiV1 host{};
    host.struct_size = sizeof(host);
    host.abi_version = PRISM_APP_ABI_V1;
    host.context = &fixture;
    host.set_binding = Fixture::AbiBinding;
    host.backend_ready = Fixture::AbiReady;
    host.schedule_tick = Fixture::AbiTick;
    host.subscribe_layout = Fixture::AbiSubscribe;
    host.control_gesture = Fixture::AbiControl;
    PrismAppInitV1 init{};
    init.struct_size = sizeof(init);
    init.abi_version = PRISM_APP_ABI_V1;
    init.host = &host;
    launch::AppModule module(module_path);
    const auto &api = module.Api();
    auto *instance = api.create(&init);
    assert(instance && !api.on_tick);

    PrismLayoutOutputV1 output{};
    output.id = 20;
    output.supported = 1;
    PrismLayoutWorkspaceV1 workspace{};
    workspace.id = 30;
    workspace.root = 40;
    workspace.output = 20;
    workspace.active = 1;
    PrismLayoutBoundaryV1 boundary{};
    boundary.id = 50;
    boundary.workspace = 30;
    boundary.visible = boundary.resizable = 1;
    PrismLayoutControlHandleV1 handle{sizeof(handle), 50, {488, 278, 48, 48}, 1};
    PrismLayoutStateV1 state{};
    state.struct_size = sizeof(state);
    state.subscription_id = 91;
    state.wm_session = 10;
    state.revision = 1;
    state.outputs = &output;
    state.outputs_size = 1;
    state.workspaces = &workspace;
    state.workspaces_size = 1;
    state.boundaries = &boundary;
    state.boundaries_size = 1;
    state.control_handle = &handle;
    api.on_layout_state(instance, &state);
    assert(fixture.Visible());

    ++state.revision;
    state.struct_size = offsetof(PrismLayoutStateV1, control_handle);
    state.control_handle = reinterpret_cast<const PrismLayoutControlHandleV1 *>(1);
    api.on_layout_state(instance, &state);
    assert(!fixture.Visible());

    ++state.revision;
    state.struct_size = sizeof(state);
    state.control_handle = &handle;
    handle.struct_size = offsetof(PrismLayoutControlHandleV1, node) - 1;
    api.on_layout_state(instance, &state);
    assert(!fixture.Visible());

    ++state.revision;
    state.control_handle = nullptr;
    api.on_layout_state(instance, &state);
    assert(!fixture.Visible());
    api.destroy(instance);
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2);
    CheckUi();
    CheckOptionalAbiTail(argv[1]);

    Fixture fixture;
    sdk::ModuleSession module(argv[1], "prism_layout_controls", 42,
                              std::bind_front(&Fixture::Binding, &fixture), {}, {}, {}, {}, {}, {},
                              {}, std::bind_front(&Fixture::Subscribe, &fixture),
                              std::bind_front(&Fixture::Send, &fixture));
    assert(module.Start() && module.BackendReady());
    assert(fixture.subscriptions == 1 && !fixture.Visible());
    assert(module.TimeoutMs(100, -1) == -1);

    module.Gesture(Begin(1));
    assert(fixture.requests.empty());
    auto layout = Layout();
    module.Deliver(layout);
    assert(fixture.Visible() && fixture.Number("handle_width") == 4 &&
           fixture.Number("handle_height") == 32);
    auto gesture = Begin(2);
    gesture.touch = true;
    module.Gesture(gesture);
    gesture = Begin(3);
    gesture.action = "unrelated";
    module.Gesture(gesture);
    module.Action("boundary:resize");
    assert(fixture.requests.empty());

    CheckContinuousResize(module, fixture, layout);
    CheckInvalidation(module, fixture, layout);
    CheckPendingEnd(module, fixture);
    module.Gesture(Begin(11));
    ConfirmLatest(module, fixture, contracts::LayoutControlStatus::Began);
    module.Disconnected();
    assert(!fixture.Visible());
    const auto disconnected_count = fixture.requests.size();
    module.Gesture(Begin(12));
    assert(fixture.requests.size() == disconnected_count);

    Fixture window;
    sdk::ModuleSession window_module(argv[1], "prism_layout_controls", 44,
                                     std::bind_front(&Fixture::Binding, &window), {}, {}, {}, {},
                                     {}, {}, {}, std::bind_front(&Fixture::Subscribe, &window),
                                     std::bind_front(&Fixture::Send, &window));
    assert(window_module.Start());
    CheckWindowModule(window_module, window);

    Fixture standalone;
    sdk::ModuleSession standalone_module(argv[1], "prism_layout_controls", 43,
                                         std::bind_front(&Fixture::Binding, &standalone));
    assert(standalone_module.Start() && standalone_module.BackendReady());
    standalone_module.Gesture(Begin(13));
    assert(!standalone.Visible() && standalone.requests.empty());
}
