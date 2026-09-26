#include "prism/launch/stream.hpp"
#include "prism/launch/worker_protocol.hpp"
#include <cassert>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace prism::launch;
using namespace prism::contracts;
int main() {
    int pair[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    Stream stream(pair[0], FrameSize);
    const auto request = EncodeMessage(LaunchRequest{{7}, "demo_player"});
    for (std::size_t i = 0; i + 1 < request.size(); ++i) {
        assert(send(pair[1], &request[i], 1, 0) == 1);
        assert(stream.Receive().empty());
    }
    assert(send(pair[1], &request.back(), 1, 0) == 1);
    auto frames = stream.Receive(); assert(frames.size() == 1 && frames[0] == request);
    auto cancel = EncodeMessage(LaunchCancel{{7}});
    assert(cancel.size() == kLaunchHeaderSize && cancel[7] == 3);
    assert(std::get<LaunchCancel>(DecodeMessage(cancel)).request.value == 7);
    for (const auto& frame : {request, cancel}) assert(send(pair[1], frame.data(), frame.size(), 0) == static_cast<ssize_t>(frame.size()));
    frames = stream.Receive(); assert(frames.size() == 2 && frames[1] == cancel);
    close(pair[1]); assert(stream.Receive().empty() && stream.Closed());

    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    Stream sender(pair[0], FrameSize);
    Stream receiver(pair[1], FrameSize);
    int size = 1024; assert(!setsockopt(sender.Fd(), SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)));
    LaunchEvent event{{8}, {9}, 42, LaunchMilestone::BackendReady, LaunchError::None, 0, {}}; event.detail.assign(2048, 'x');
    auto large = EncodeMessage(event);
    for (int i = 0; i < 40; ++i) assert(sender.Queue(large));
    sender.Flush(); assert(sender.WantsWrite());
    unsigned received = 0;
    for (int turn = 0; turn < 200 && received != 40; ++turn) {
        for (const auto& frame : receiver.Receive()) { assert(frame == large); ++received; }
        sender.Flush();
    }
    assert(received == 40 && !sender.WantsWrite());
    for (int i = 0; i < 200; ++i) if (!sender.Queue(large)) break;
    assert(sender.Closed()); // Bound a slow reader's pending bytes.

    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    Stream invalid(pair[0], FrameSize);
    auto bad = request; bad[0] = 0;
    assert(send(pair[1], bad.data(), bad.size(), 0) == static_cast<ssize_t>(bad.size()));
    bool rejected = false;
    try { invalid.Receive(); } catch (...) { rejected = true; }
    assert(rejected && invalid.Closed()); close(pair[1]);

    const WorkerBind bind{LaunchRequest{{3}, "demo_player", LaunchMode::NewInstance}, {5}};
    auto worker = EncodeWorker(bind);
    assert(std::get<WorkerBind>(DecodeWorker(worker)).instance.value == 5);
    assert(std::get<WorkerReady>(DecodeWorker(EncodeWorker(WorkerReady{17}))).preparation_ns == 17);
    assert(std::get<WorkerReply>(DecodeWorker(EncodeWorker(WorkerReply{event}))).event == event);
    for (std::size_t n = 0; n < worker.size(); ++n)
        assert(WorkerFrameSize(std::span(worker).first(n)) == (n < 12 ? 0 : worker.size()));
    bad = worker; bad[7] = 0;
    rejected = false; try { DecodeWorker(bad); } catch (...) { rejected = true; }
    assert(rejected);
}
