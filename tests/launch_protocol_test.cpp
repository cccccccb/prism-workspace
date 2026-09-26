#include "prism/launch/protocol.hpp"
#include "prism/launch/instance_state.hpp"
#include <cassert>
#include <functional>
#include <stdexcept>

using namespace prism::contracts;
using namespace prism::launch;
void Reject(const std::function<void()>& fn) {
    bool rejected = false;
    try { fn(); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}
LaunchEvent Event(LaunchMilestone milestone, std::uint32_t pid = 42) {
    LaunchEvent event;
    event.request = {7}; event.instance = {9}; event.pid = pid; event.milestone = milestone;
    return event;
}
int main() {
    const LaunchRequest request{{7}, "music", LaunchMode::NewInstance};
    const std::vector<std::uint8_t> golden{
        0x50,0x52,0x4c,0x31, 0,1, 0,1, 0,0,0,8,
        0,0,0,0,0,0,0,7, 0,0,0,0,0,0,0,0,
        1,0,5,'m','u','s','i','c'};
    const auto encoded = EncodeMessage(request);
    assert(encoded == golden);
    assert(std::get<LaunchRequest>(DecodeMessage(golden)) == request);
    for (std::size_t n = 0; n < golden.size(); ++n) {
        const auto part = std::span(golden).first(n);
        assert(FrameSize(part) == (n < kLaunchHeaderSize ? 0 : golden.size()));
        Reject([&] { DecodeMessage(part); });
    }
    for (auto offset : {0, 5, 7, 27, 28}) {
        auto bad = golden; bad[offset] = 99;
        Reject([&] { DecodeMessage(bad); });
    }
    auto large = golden;
    large[8] = 1;
    Reject([&] { FrameSize(large); });
    auto trailing = golden; trailing.push_back(0);
    Reject([&] { DecodeMessage(trailing); });
    auto body = golden; body[30] = 99;
    Reject([&] { DecodeMessage(body); });
    Reject([] { EncodeMessage(LaunchRequest{{0}, "music"}); });
    Reject([] { EncodeMessage(LaunchRequest{{1}, "sh -c evil"}); });
    auto exited = Event(LaunchMilestone::Exited); exited.exit_code = -15;
    assert(std::get<LaunchEvent>(DecodeMessage(EncodeMessage(exited))) == exited);
    auto event = Event(LaunchMilestone::BackendReady); event.detail = "业务就绪";
    assert(std::get<LaunchEvent>(DecodeMessage(EncodeMessage(event))) == event);
    event.error = LaunchError::Timeout;
    Reject([&] { EncodeMessage(event); });
    event = Event(LaunchMilestone::Failed); event.error = LaunchError::RuntimeFailed;
    assert(std::get<LaunchEvent>(DecodeMessage(EncodeMessage(event))) == event);
    event.detail = std::string("\xc0\x80", 2);
    Reject([&] { EncodeMessage(event); });
    event.detail.assign(2049, 'a');
    Reject([&] { EncodeMessage(event); });
    event.detail.clear(); event.instance = {0}; event.pid = 0;
    assert(std::get<LaunchEvent>(DecodeMessage(EncodeMessage(event))) == event);

    for (bool backend_first : {false, true}) {
        InstanceState state({7}, {9});
        assert(!state.Apply(Event(LaunchMilestone::FirstPresented)));
        assert(state.Apply(Event(LaunchMilestone::Accepted, 0)));
        assert(!state.Apply(Event(LaunchMilestone::Accepted, 0)));
        assert(state.Apply(Event(LaunchMilestone::WorkerAssigned)));
        assert(!state.Apply(Event(LaunchMilestone::BackendReady)));
        auto wrong = Event(LaunchMilestone::RuntimeReady); wrong.instance = {8};
        assert(!state.Apply(wrong));
        assert(!state.Apply(Event(LaunchMilestone::RuntimeReady, 43)));
        assert(state.Apply(Event(LaunchMilestone::RuntimeReady)));
        if (backend_first) assert(state.Apply(Event(LaunchMilestone::BackendReady)));
        assert(state.Apply(Event(LaunchMilestone::SurfaceConfigured)));
        assert(state.Apply(Event(LaunchMilestone::FirstPresented)));
        if (!backend_first) assert(state.Apply(Event(LaunchMilestone::BackendReady)));
        assert(state.FirstPresented() && state.BackendReady());
        assert(!state.Apply(Event(LaunchMilestone::BackendReady)));
        assert(state.Apply(exited));
        assert(state.Terminal() && !state.Apply(Event(LaunchMilestone::RuntimeReady)));
    }
    InstanceState failed({7}, {9});
    assert(failed.Apply(Event(LaunchMilestone::Accepted, 0)));
    assert(failed.Apply(Event(LaunchMilestone::WorkerAssigned)));
    auto failure = Event(LaunchMilestone::Failed); failure.error = LaunchError::Cancelled;
    assert(failed.Apply(failure));
    assert(!failed.Apply(Event(LaunchMilestone::RuntimeReady)));
    assert(failed.Apply(exited)); // Failure must not suppress eventual process reaping.
    assert(failed.Error() == LaunchError::Cancelled && failed.Terminal());
    assert(failed.ExitCode() == -15);
}
