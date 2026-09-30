#include "prism/sdk/layout_control_bridge.hpp"
#include <cassert>
#include <functional>
#include <vector>

using namespace prism::contracts;
using prism::sdk::LayoutControlBridge;

namespace {
struct Transport {
    std::vector<LayoutControlRequest> sent;
    bool available{true};

    bool Send(const LayoutControlRequest &request)
    {
        if (available) {
            sent.push_back(request);
        }
        return available;
    }
};

GestureEvent Gesture(std::uint64_t id, GesturePhase phase, double x)
{
    GestureEvent event;
    event.id = id;
    event.phase = phase;
    event.serial = 23;
    event.source = {1, 2, 3};
    event.node = {4, 1};
    event.action = "group";
    event.position = {x, 4};
    return event;
}

const LayoutControlTarget target{11, 12, 13, 14, 0, 15, 16};

LayoutControlResult Reply(const LayoutControlRequest &request, LayoutControlStatus status,
                          LayoutControlError error = LayoutControlError::None)
{
    LayoutControlResult result;
    result.request = request.request;
    result.gesture = request.gesture;
    result.sequence = request.sequence;
    result.session = request.session ? request.session : 20;
    result.status = status;
    result.error = error;
    result.position = request.position;
    return result;
}

void Observe(LayoutControlBridge &bridge, std::uint64_t id, GesturePhase phase, double x)
{
    bridge.Observe(Gesture(id, phase, x));
    bridge.Advance();
}

void PendingTerminal()
{
    Transport transport;
    LayoutControlBridge bridge(std::bind_front(&Transport::Send, &transport));
    assert(!bridge.Begin(1, LayoutControlOperation::GroupGesture, target));
    bridge.Observe(Gesture(1, GesturePhase::Begin, 1));
    assert(bridge.Begin(1, LayoutControlOperation::GroupGesture, target));
    assert(!bridge.Begin(1, LayoutControlOperation::GroupGesture, target));
    bridge.Advance();
    assert(transport.sent.size() == 1);
    assert(transport.sent[0].input.serial == 23);
    assert(transport.sent[0].session == 0 && transport.sent[0].sequence == 1);

    Observe(bridge, 1, GesturePhase::Update, 2);
    Observe(bridge, 1, GesturePhase::Update, 3);
    bridge.Observe(Gesture(1, GesturePhase::End, 4));
    assert(bridge.EndIntent(1, LayoutControlIntent::EnterImmersive));
    bridge.Advance();
    assert(transport.sent.size() == 1);

    const auto began = Reply(transport.sent[0], LayoutControlStatus::Began);
    bridge.Receive(began);
    assert(transport.sent.size() == 2);
    assert(transport.sent[1].session == 20 && transport.sent[1].sequence == 2);
    assert(transport.sent[1].position.x == 3);
    assert(transport.sent[1].phase == LayoutControlPhase::Update);
    bridge.Receive(began);
    assert(transport.sent.size() == 2);
    bridge.Receive(Reply(transport.sent[1], LayoutControlStatus::Updated));
    assert(transport.sent.size() == 3);
    assert(transport.sent[2].position.x == 4 && transport.sent[2].sequence == 3);
    assert(transport.sent[2].phase == LayoutControlPhase::End);
    assert(transport.sent[2].intent == LayoutControlIntent::EnterImmersive);
    bridge.Receive(
        Reply(transport.sent[2], LayoutControlStatus::Rejected, LayoutControlError::Unsupported));
    assert(bridge.ActiveCount() == 0);
    assert(bridge.TakeResults().size() == 3);
    bridge.Receive(Reply(transport.sent[2], LayoutControlStatus::Ended));
    assert(bridge.TakeResults().empty());
}

void Cancellation()
{
    Transport transport;
    LayoutControlBridge bridge(std::bind_front(&Transport::Send, &transport));
    bridge.Observe(Gesture(2, GesturePhase::Begin, 1));
    assert(bridge.Begin(2, LayoutControlOperation::GroupGesture, target));
    bridge.Advance();
    Observe(bridge, 2, GesturePhase::Update, 2);
    assert(bridge.Cancel(2));
    assert(!bridge.Cancel(2));
    bridge.Receive(Reply(transport.sent.back(), LayoutControlStatus::Began));
    assert(transport.sent.size() == 2);
    assert(transport.sent.back().phase == LayoutControlPhase::Cancel);
    assert(transport.sent.back().sequence == 2);
    bridge.Receive(Reply(transport.sent.back(), LayoutControlStatus::Cancelled));
    assert(bridge.ActiveCount() == 0);

    bridge.Observe(Gesture(3, GesturePhase::Begin, 1));
    assert(bridge.Begin(3, LayoutControlOperation::GroupGesture, target));
    bridge.Advance();
    const auto began = Reply(transport.sent.back(), LayoutControlStatus::Began);
    bridge.Receive(began);
    Observe(bridge, 3, GesturePhase::Update, 3);
    auto external = began;
    external.status = LayoutControlStatus::Cancelled;
    external.error = LayoutControlError::StaleLayout;
    bridge.Receive(external);
    assert(bridge.ActiveCount() == 0);
    bridge.TakeResults();
    bridge.Receive(Reply(transport.sent.back(), LayoutControlStatus::Updated));
    assert(bridge.TakeResults().empty());
}

void CapacityAndFailure()
{
    Transport transport;
    LayoutControlBridge bridge(std::bind_front(&Transport::Send, &transport));
    for (std::uint64_t id = 1; id <= 17; ++id) {
        bridge.Observe(Gesture(id, GesturePhase::Begin, 1));
        assert(bridge.Begin(id, LayoutControlOperation::GroupGesture, target) == (id <= 16));
        bridge.Advance();
    }
    assert(bridge.ActiveCount() == 16);
    bridge.Disconnect();
    assert(bridge.ActiveCount() == 0 && bridge.TakeResults().size() == 16);
    bridge.Disconnect();
    assert(bridge.TakeResults().empty());
    bridge.Observe(Gesture(18, GesturePhase::Begin, 1));
    assert(!bridge.Begin(18, LayoutControlOperation::GroupGesture, target));

    LayoutControlBridge failing(std::bind_front(&Transport::Send, &transport));
    failing.Observe(Gesture(30, GesturePhase::Begin, 1));
    assert(failing.Begin(30, LayoutControlOperation::GroupGesture, target));
    failing.Advance();
    const auto began = Reply(transport.sent.back(), LayoutControlStatus::Began);
    transport.available = false;
    Observe(failing, 30, GesturePhase::Update, 3);
    failing.Receive(began);
    const auto results = failing.TakeResults();
    assert(results.size() == 2 && failing.ActiveCount() == 0);
    assert(results.back().error == LayoutControlError::Disconnected);
}
} // namespace

int main()
{
    PendingTerminal();
    Cancellation();
    CapacityAndFailure();
}
