#include "prism/sdk/module_session.hpp"
#include <cassert>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace prism;

namespace {
struct Fixture {
    std::map<std::string, double, std::less<>> values;
    std::vector<contracts::LayoutControlRequest> requests;

    bool Binding(std::string_view key, runtime::PropertyValue value)
    {
        values.insert_or_assign(std::string(key), std::get<double>(value));
        return true;
    }

    std::uint64_t Subscribe(bool enabled)
    {
        assert(enabled);
        return 44;
    }

    bool Send(const contracts::LayoutControlRequest &request)
    {
        requests.push_back(request);
        return true;
    }
};

contracts::LayoutControlResult Result(const contracts::LayoutControlRequest &request,
                                      contracts::LayoutControlStatus status)
{
    contracts::LayoutControlResult result;
    result.request = request.request;
    result.gesture = request.gesture;
    result.sequence = request.sequence;
    result.session = 25;
    result.status = status;
    return result;
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2);
    Fixture fixture;
    sdk::ModuleSession module(argv[1], "control-fixture", 42,
                              std::bind_front(&Fixture::Binding, &fixture), {}, {}, {}, {}, {}, {},
                              {}, std::bind_front(&Fixture::Subscribe, &fixture),
                              std::bind_front(&Fixture::Send, &fixture));
    assert(module.Start() && module.BackendReady());

    contracts::LayoutStateEvent layout;
    layout.subscription = 44;
    auto &snapshot = layout.snapshot;
    snapshot.session = 1;
    snapshot.revision = 2;
    snapshot.topology_revision = 3;
    snapshot.layout_revision = 4;
    snapshot.focus_revision = 5;
    snapshot.outputs.push_back({6, "virtual", {0, 0, 640, 480}, 1.0, true, true});
    snapshot.workspaces.push_back({7, 8, 6, "workspace", true});
    contracts::LayoutNode node;
    node.id = 8;
    node.workspace = 7;
    snapshot.nodes.push_back(node);
    module.Deliver(layout);
    assert(fixture.values.at("layout_status") == 0);
    assert(fixture.values.at("output_count") == 1);
    assert(fixture.values.at("node_count") == 1);

    contracts::GestureEvent gesture;
    gesture.id = 9;
    gesture.serial = 10;
    gesture.node = {2, 1};
    gesture.source = {1, 2, 3};
    gesture.action = "control-group";
    gesture.position = {20, 30};
    gesture.snapshot_scene = 33;
    gesture.snapshot_version = 34;
    module.Gesture(gesture);
    assert(fixture.values.at("command_result") == 0);
    assert(fixture.values.at("gesture_serial") == 10);
    assert(fixture.values.at("bad_phase") == -1);
    assert(fixture.requests.size() == 1);
    assert(fixture.requests[0].target.root == 8 && fixture.requests[0].input.serial == 10);

    gesture.phase = contracts::GesturePhase::Update;
    gesture.position.x = 40;
    module.Gesture(gesture);
    gesture.position.x = 50;
    module.Gesture(gesture);
    gesture.phase = contracts::GesturePhase::End;
    module.Gesture(gesture);
    assert(fixture.requests.size() == 1);
    assert(fixture.values.at("bad_intent") == -1);
    module.Deliver(Result(fixture.requests[0], contracts::LayoutControlStatus::Began));
    assert(fixture.requests.size() == 2 && fixture.requests[1].position.x == 50);
    module.Deliver(Result(fixture.requests[1], contracts::LayoutControlStatus::Updated));
    assert(fixture.requests.size() == 3);
    assert(fixture.requests[2].phase == contracts::LayoutControlPhase::End);
    module.Deliver(Result(fixture.requests[2], contracts::LayoutControlStatus::Ended));
    assert(fixture.values.at("result_count") == 3);
    assert(fixture.values.at("result_status") == static_cast<double>(PRISM_LAYOUT_ENDED_V1));
    assert(fixture.values.at("result_applied") == 0);

    gesture.id = 11;
    gesture.phase = contracts::GesturePhase::Begin;
    module.Gesture(gesture);
    module.Disconnected();
    assert(fixture.values.at("result_count") == 4);
    assert(fixture.values.at("result_error") == static_cast<double>(PRISM_LAYOUT_DISCONNECTED_V1));
    assert(fixture.values.at("layout_status") == 2);
    module.Disconnected();
    assert(fixture.values.at("result_count") == 4);
    gesture.id = 12;
    module.Gesture(gesture);
    assert(fixture.values.at("command_result") == -1);
}
