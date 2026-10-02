#include "prism/sdk/module_session.hpp"
#include <cassert>
#include <cstdint>
#include <functional>
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

    double ImmersiveOpacity() const
    {
        return std::get<double>(bindings.at("group_immersive_opacity"));
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
    contracts::LayoutNode root;
    root.id = 40;
    root.workspace = 30;
    state.nodes.push_back(root);
    return event;
}

contracts::GestureEvent Begin(std::uint64_t id)
{
    contracts::GestureEvent event;
    event.id = id;
    event.node = {3, 1};
    event.action = "group:toggle-immersive";
    event.source = {1, 2, 3};
    event.serial = 88;
    event.start = {512, 6};
    event.position = {512, 13};
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

void CompleteGesture(sdk::ModuleSession &module, Fixture &fixture, std::uint64_t id, double dx,
                     double dy, contracts::LayoutControlIntent intent)
{
    const auto before = fixture.requests.size();
    const auto opacity = fixture.ImmersiveOpacity();
    auto gesture = Begin(id);
    module.Gesture(gesture);
    assert(fixture.requests.size() == before + 1);
    const auto begin = fixture.requests.back();
    assert(begin.phase == contracts::LayoutControlPhase::Begin);
    assert(begin.operation == contracts::LayoutControlOperation::GroupGesture);
    assert(begin.target.workspace == 30 && begin.target.root == 40 && begin.target.output == 20);
    assert(begin.target.wm_session == 10 && begin.input.serial == 88);
    module.Deliver(Result(begin, contracts::LayoutControlStatus::Began));

    gesture.phase = contracts::GesturePhase::End;
    gesture.position = {gesture.start.x + dx, gesture.start.y + dy};
    module.Gesture(gesture);
    assert(fixture.requests.size() == before + 2);
    const auto end = fixture.requests.back();
    assert(end.phase == contracts::LayoutControlPhase::End && end.intent == intent);
    auto result = Result(end, contracts::LayoutControlStatus::Ended);
    result.applied = intent != contracts::LayoutControlIntent::None;
    module.Deliver(result);
    assert(fixture.ImmersiveOpacity() == opacity);
}

void RejectedAndCancelled(sdk::ModuleSession &module, Fixture &fixture,
                          contracts::LayoutStateEvent &layout)
{
    auto gesture = Begin(20);
    module.Gesture(gesture);
    const auto rejected_count = fixture.requests.size();
    auto rejected = Result(fixture.requests.back(), contracts::LayoutControlStatus::Rejected);
    rejected.error = contracts::LayoutControlError::StaleLayout;
    module.Deliver(rejected);
    gesture.phase = contracts::GesturePhase::End;
    gesture.position.y += 60;
    module.Gesture(gesture);
    assert(fixture.requests.size() == rejected_count && fixture.ImmersiveOpacity() == 0);

    gesture = Begin(21);
    module.Gesture(gesture);
    module.Deliver(Result(fixture.requests.back(), contracts::LayoutControlStatus::Began));
    gesture.phase = contracts::GesturePhase::Cancel;
    module.Gesture(gesture);
    assert(fixture.requests.back().phase == contracts::LayoutControlPhase::Cancel);
    module.Deliver(Result(fixture.requests.back(), contracts::LayoutControlStatus::Cancelled));

    gesture = Begin(22);
    module.Gesture(gesture);
    module.Deliver(Result(fixture.requests.back(), contracts::LayoutControlStatus::Began));
    const auto tracked = fixture.requests.size();
    ++layout.snapshot.revision;
    ++layout.snapshot.focus_revision;
    module.Deliver(layout);
    assert(fixture.requests.size() == tracked);

    ++layout.snapshot.revision;
    ++layout.snapshot.layout_revision;
    module.Deliver(layout);
    assert(fixture.requests.size() == tracked + 1);
    assert(fixture.requests.back().phase == contracts::LayoutControlPhase::Cancel);
    module.Deliver(Result(fixture.requests.back(), contracts::LayoutControlStatus::Cancelled));
    gesture.phase = contracts::GesturePhase::End;
    gesture.position.y += 60;
    module.Gesture(gesture);
    assert(fixture.requests.size() == tracked + 1 && fixture.ImmersiveOpacity() == 0);
}

void ReleaseBeforeAcknowledgement(sdk::ModuleSession &module, Fixture &fixture)
{
    const auto before = fixture.requests.size();
    auto gesture = Begin(26);
    module.Gesture(gesture);
    assert(fixture.requests.size() == before + 1);
    const auto begin = fixture.requests.back();

    gesture.phase = contracts::GesturePhase::End;
    gesture.position.y += 40;
    module.Gesture(gesture);
    module.Gesture(Begin(27));
    assert(fixture.requests.size() == before + 1);
    module.Deliver(Result(begin, contracts::LayoutControlStatus::Began));
    assert(fixture.requests.size() == before + 2);
    const auto end = fixture.requests.back();
    assert(end.phase == contracts::LayoutControlPhase::End);
    assert(end.intent == contracts::LayoutControlIntent::EnterImmersive);
    assert(fixture.ImmersiveOpacity() == 0);
    module.Deliver(Result(end, contracts::LayoutControlStatus::Ended));
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2);
    Fixture fixture;
    sdk::ModuleSession module(argv[1], "prism_topbar", 42,
                              std::bind_front(&Fixture::Binding, &fixture), {}, {}, {}, {}, {}, {},
                              {}, std::bind_front(&Fixture::Subscribe, &fixture),
                              std::bind_front(&Fixture::Send, &fixture));
    assert(module.Start() && module.BackendReady());
    assert(fixture.subscriptions == 1 && fixture.ImmersiveOpacity() == 0);

    module.Gesture(Begin(1));
    assert(fixture.requests.empty());
    auto layout = Layout();
    module.Deliver(layout);
    auto gesture = Begin(2);
    gesture.touch = true;
    module.Gesture(gesture);
    gesture = Begin(3);
    gesture.action = "unrelated";
    module.Gesture(gesture);
    module.Action("group:toggle-immersive");
    assert(fixture.requests.empty());

    using Intent = contracts::LayoutControlIntent;
    CompleteGesture(module, fixture, 4, 0, 23.9, Intent::None);
    CompleteGesture(module, fixture, 5, 0, -100, Intent::None);
    CompleteGesture(module, fixture, 6, 25, 24, Intent::None);
    CompleteGesture(module, fixture, 7, 24, 24, Intent::EnterImmersive);
    assert(fixture.ImmersiveOpacity() == 0);

    ++layout.snapshot.revision;
    ++layout.snapshot.layout_revision;
    auto &workspace = layout.snapshot.workspaces.front();
    workspace.mode = contracts::LayoutGroupMode::Immersive;
    ++workspace.mode_revision;
    module.Deliver(layout);
    assert(fixture.ImmersiveOpacity() == 1);
    module.Deliver(Layout());
    assert(fixture.ImmersiveOpacity() == 1);
    CompleteGesture(module, fixture, 8, 0, 40, Intent::ExitImmersive);
    assert(fixture.ImmersiveOpacity() == 1);

    ++layout.snapshot.revision;
    ++layout.snapshot.layout_revision;
    workspace.mode = contracts::LayoutGroupMode::Normal;
    ++workspace.mode_revision;
    module.Deliver(layout);
    assert(fixture.ImmersiveOpacity() == 0);
    RejectedAndCancelled(module, fixture, layout);
    ReleaseBeforeAcknowledgement(module, fixture);

    module.Gesture(Begin(23));
    module.Deliver(Result(fixture.requests.back(), contracts::LayoutControlStatus::Began));
    module.Disconnected();
    const auto disconnected_count = fixture.requests.size();
    module.Gesture(Begin(24));
    assert(fixture.requests.size() == disconnected_count);

    Fixture standalone;
    sdk::ModuleSession standalone_module(argv[1], "prism_topbar", 43,
                                         std::bind_front(&Fixture::Binding, &standalone));
    assert(standalone_module.Start() && standalone_module.BackendReady());
    standalone_module.Gesture(Begin(25));
    assert(standalone.ImmersiveOpacity() == 0 && standalone.requests.empty());
}
