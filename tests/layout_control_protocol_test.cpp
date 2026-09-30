#include "prism/contracts/layout_control.hpp"
#include "prism/launch/control_protocol.hpp"
#include "prism/launch/stream.hpp"
#include "prism/launch/worker_protocol.hpp"
#include <cassert>
#include <functional>
#include <limits>
#include <stdexcept>
#include <sys/socket.h>

using namespace prism;
using namespace contracts;

namespace {
void Reject(const std::function<void()> &operation)
{
    bool rejected{};
    try {
        operation();
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
}

LayoutControlRequest Request()
{
    LayoutControlRequest request;
    request.request = 81;
    request.gesture = 71;
    request.sequence = 1;
    request.target = {12, 23, 34, 45, 0, 9, 11};
    request.input = {LayoutInputKind::Pointer, 987, 0};
    request.position = {23.75, 10.5};
    return request;
}

LayoutSnapshot Snapshot()
{
    LayoutSnapshot snapshot;
    snapshot.session = 12;
    snapshot.revision = snapshot.layout_revision = snapshot.topology_revision =
        snapshot.focus_revision = 1;
    return snapshot;
}

void Requests()
{
    auto request = Request();
    auto bytes = EncodeLayoutControl(request);
    assert(bytes.size() == kLayoutControlPayload);
    assert(DecodeLayoutControl(bytes) == request);
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        Reject([&] { DecodeLayoutControl(std::span(bytes).first(size)); });
    }
    auto malformed = bytes;
    malformed.push_back(0);
    Reject([&] { DecodeLayoutControl(malformed); });
    malformed = bytes;
    malformed[1] = 2;
    Reject([&] { DecodeLayoutControl(malformed); });
    malformed = bytes;
    malformed[34] = 99;
    Reject([&] { DecodeLayoutControl(malformed); });

    auto invalid = request;
    invalid.request = 0;
    Reject([&] { EncodeLayoutControl(invalid); });
    invalid = request;
    invalid.gesture = 0;
    Reject([&] { EncodeLayoutControl(invalid); });
    invalid = request;
    invalid.sequence = 2;
    Reject([&] { EncodeLayoutControl(invalid); });
    invalid = request;
    invalid.session = 1;
    Reject([&] { EncodeLayoutControl(invalid); });
    invalid = request;
    invalid.phase = LayoutControlPhase::Update;
    Reject([&] { EncodeLayoutControl(invalid); });
    invalid = request;
    invalid.target.topology_revision = 0;
    Reject([&] { EncodeLayoutControl(invalid); });
    invalid = request;
    invalid.target.boundary = 8;
    Reject([&] { EncodeLayoutControl(invalid); });
    invalid = request;
    invalid.operation = LayoutControlOperation::BoundaryGesture;
    Reject([&] { EncodeLayoutControl(invalid); });
    invalid = request;
    invalid.intent = LayoutControlIntent::EnterImmersive;
    Reject([&] { EncodeLayoutControl(invalid); });
    invalid = request;
    invalid.position.x = std::numeric_limits<double>::quiet_NaN();
    Reject([&] { EncodeLayoutControl(invalid); });
    invalid.position.x = std::numeric_limits<double>::infinity();
    Reject([&] { EncodeLayoutControl(invalid); });
    invalid.position.x = kMaxLayoutControlCoordinate + 1;
    Reject([&] { EncodeLayoutControl(invalid); });
    invalid = request;
    invalid.input.contact = -1;
    Reject([&] { EncodeLayoutControl(invalid); });
    invalid.input.contact = 1;
    Reject([&] { EncodeLayoutControl(invalid); });

    request.input = {LayoutInputKind::Touch, 33, 4};
    assert(DecodeLayoutControl(EncodeLayoutControl(request)) == request);
    // Missing platform credentials reach the WM as a typed InvalidInput result.
    request.input.serial = 0;
    assert(DecodeLayoutControl(EncodeLayoutControl(request)) == request);
    request.session = 20;
    request.sequence = 2;
    for (const auto phase :
         {LayoutControlPhase::Update, LayoutControlPhase::End, LayoutControlPhase::Cancel}) {
        request.phase = phase;
        assert(DecodeLayoutControl(EncodeLayoutControl(request)) == request);
    }
    request.phase = LayoutControlPhase::End;
    request.intent = LayoutControlIntent::EnterImmersive;
    assert(DecodeLayoutControl(EncodeLayoutControl(request)) == request);
    request.intent = LayoutControlIntent::ApplyBoundary;
    Reject([&] { EncodeLayoutControl(request); });
    request.operation = LayoutControlOperation::BoundaryGesture;
    request.target.boundary = 19;
    assert(DecodeLayoutControl(EncodeLayoutControl(request)) == request);
}

void Results()
{
    LayoutControlResult result;
    result.request = 81;
    result.gesture = 71;
    result.sequence = 1;
    result.error = LayoutControlError::Unauthorized;
    assert(DecodeLayoutControlResult(EncodeLayoutControlResult(result)) == result);
    result.session = 20;
    result.status = LayoutControlStatus::Began;
    result.error = LayoutControlError::None;
    result.revision = 14;
    result.topology_revision = 9;
    result.layout_revision = 11;
    result.position = {20, 40};
    const auto bytes = EncodeLayoutControlResult(result);
    assert(bytes.size() == kLayoutControlResultPayload);
    assert(DecodeLayoutControlResult(bytes) == result);
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        Reject([&] { DecodeLayoutControlResult(std::span(bytes).first(size)); });
    }
    auto malformed = bytes;
    malformed.back() = 2;
    Reject([&] { DecodeLayoutControlResult(malformed); });
    malformed = bytes;
    malformed[35] = 255;
    Reject([&] { DecodeLayoutControlResult(malformed); });
    result.applied = true;
    Reject([&] { EncodeLayoutControlResult(result); });
    result.status = LayoutControlStatus::Ended;
    assert(DecodeLayoutControlResult(EncodeLayoutControlResult(result)) == result);
}

void Subscriptions()
{
    for (const bool enabled : {true, false}) {
        const LayoutSubscription request{81, enabled};
        const auto bytes = EncodeLayoutSubscription(request);
        assert(DecodeLayoutSubscription(bytes) == request);
        auto malformed = bytes;
        malformed.back() = 2;
        Reject([&] { DecodeLayoutSubscription(malformed); });
    }
    Reject([] { EncodeLayoutSubscription({}); });

    LayoutStateEvent event{81, LayoutStateStatus::Current, Snapshot()};
    const auto bytes = EncodeLayoutState(event);
    assert(DecodeLayoutState(bytes) == event);
    for (const auto status : {LayoutStateStatus::Denied, LayoutStateStatus::Disconnected}) {
        event.status = status;
        Reject([&] { EncodeLayoutState(event); });
        event.snapshot = {};
        auto error_bytes = EncodeLayoutState(event);
        assert(error_bytes.size() == 11 && DecodeLayoutState(error_bytes) == event);
        error_bytes.push_back(0);
        Reject([&] { DecodeLayoutState(error_bytes); });
        event.snapshot = Snapshot();
    }
    auto malformed = bytes;
    malformed[10] = 200;
    Reject([&] { DecodeLayoutState(malformed); });
    malformed.assign(kMaxLayoutStatePayload + 1, 0);
    Reject([&] { DecodeLayoutState(malformed); });
}

void Transports()
{
    launch::ControlMessage message;
    message.type = launch::ControlType::LayoutControl;
    message.permit.session = 12;
    message.permit.request = {123};
    message.permit.instance = {987};
    message.permit.pid = 12345;
    message.permit.role = WindowRole::TopBar;
    message.control_request = Request();
    auto bytes = launch::EncodeControl(message);
    auto decoded = launch::DecodeControl(bytes);
    assert(decoded.control_request == message.control_request);
    assert(decoded.permit.request == message.permit.request &&
           decoded.permit.session == message.permit.session &&
           decoded.permit.instance == message.permit.instance &&
           decoded.permit.pid == message.permit.pid && decoded.permit.role == message.permit.role);

    message.type = launch::ControlType::LayoutControlResult;
    message.control_result.request = 81;
    message.control_result.gesture = 71;
    message.control_result.sequence = 1;
    message.control_result.error = LayoutControlError::InvalidInput;
    bytes = launch::EncodeControl(message);
    decoded = launch::DecodeControl(bytes);
    assert(decoded.control_result == message.control_result);
    assert(decoded.permit.instance == message.permit.instance &&
           decoded.permit.pid == message.permit.pid && decoded.permit.role == message.permit.role);
    bytes[11]--;
    Reject([&] { launch::ControlFrameSize(bytes); });

    const std::vector<launch::WorkerMessage> messages{
        LayoutSubscription{81, true}, LayoutStateEvent{81, LayoutStateStatus::Current, Snapshot()},
        Request(), message.control_result};
    for (std::size_t i = 0; i < messages.size(); ++i) {
        auto frame = launch::EncodeWorker(messages[i]);
        assert(frame[7] == i + 13 && launch::WorkerFrameSize(frame) == frame.size());
        const auto result = launch::DecodeWorker(frame);
        assert(result.index() == messages[i].index());
        assert(launch::EncodeWorker(result) == frame);
        auto invalid = frame;
        invalid[8] = 1;
        Reject([&] { launch::WorkerFrameSize(invalid); });
        // The public launch endpoint cannot interpret a private worker control packet.
        Reject([&] { launch::DecodeMessage(frame); });
    }

    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    launch::Stream writer(sockets[0], launch::WorkerFrameSize);
    launch::Stream reader(sockets[1], launch::WorkerFrameSize);
    for (const auto &message : messages) {
        assert(writer.Queue(launch::EncodeWorker(message)));
    }
    writer.Flush();
    const auto received = reader.Receive();
    assert(received.size() == messages.size());
    assert(std::get<LayoutControlRequest>(launch::DecodeWorker(received[2])) == Request());
    std::vector<std::uint8_t> excessive(256 * 1024 + 1);
    assert(!writer.Queue(excessive) && writer.Closed());
}
} // namespace

int main()
{
    Requests();
    Results();
    Subscriptions();
    Transports();
}
