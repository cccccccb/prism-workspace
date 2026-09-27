#include "prism/launch/stream.hpp"
#include "prism/launch/worker_protocol.hpp"
#include <cassert>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
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

    // Receive's extraction budget leaves complete frames in userspace. They
    // remain immediate work even though no more bytes are readable from the fd.
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    Stream burst(pair[0],FrameSize);
    std::vector<std::uint8_t> batch;
    for(unsigned n=1;n<=130;++n){
        const auto frame=EncodeMessage(LaunchCancel{{n}});
        batch.insert(batch.end(),frame.begin(),frame.end());
    }
    assert(send(pair[1],batch.data(),batch.size(),0)==static_cast<ssize_t>(batch.size()));
    frames=burst.Receive();assert(frames.size()==64&&burst.HasCompleteFrame());
    pollfd empty_kernel{burst.Fd(),POLLIN,0};assert(poll(&empty_kernel,1,0)==0);
    auto retained=burst.Receive();assert(retained.size()==64&&burst.HasCompleteFrame());
    assert(std::get<LaunchCancel>(DecodeMessage(retained.front())).request.value==65);
    retained=burst.Receive();assert(retained.size()==2&&!burst.HasCompleteFrame());
    assert(std::get<LaunchCancel>(DecodeMessage(retained.back())).request.value==130);
    // A retained fragment must not select timeout zero and cause an idle spin.
    assert(send(pair[1],request.data(),request.size()-1,0)==static_cast<ssize_t>(request.size()-1));
    assert(burst.Receive().empty()&&burst.HasPartialFrame()&&!burst.HasCompleteFrame());
    assert(send(pair[1],&request.back(),1,0)==1);
    retained=burst.Receive();assert(retained.size()==1&&retained.front()==request);
    close(pair[1]);assert(burst.Receive().empty()&&burst.Closed());

    // EOF cannot discard complete frames across the same extraction boundary.
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    Stream ended(pair[0],FrameSize);
    assert(send(pair[1],batch.data(),batch.size(),0)==static_cast<ssize_t>(batch.size()));
    assert(!shutdown(pair[1],SHUT_WR));
    unsigned delivered{};
    for(unsigned turn=0;turn<4;++turn){
        const auto portion=ended.Receive();delivered+=portion.size();
        if(turn<2)assert(ended.HasCompleteFrame()&&!ended.Closed());
        if(turn==2)assert(portion.size()==2&&!ended.Closed());
    }
    assert(delivered==130&&ended.Closed());close(pair[1]);

    // Readiness runs outside endpoint parsing catches. A malformed header
    // retained after 64 good frames must wake another drain without throwing
    // through the launcher's global wait loop.
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    Stream corrupt_tail(pair[0],FrameSize);
    std::vector<std::uint8_t> malformed_batch;
    for(unsigned n=1;n<=64;++n){
        const auto frame=EncodeMessage(LaunchCancel{{n}});
        malformed_batch.insert(malformed_batch.end(),frame.begin(),frame.end());
    }
    auto malformed=request;malformed[0]=0;
    malformed_batch.insert(malformed_batch.end(),malformed.begin(),malformed.end());
    assert(send(pair[1],malformed_batch.data(),malformed_batch.size(),0)==static_cast<ssize_t>(malformed_batch.size()));
    assert(corrupt_tail.Receive().size()==64);
    assert(corrupt_tail.HasCompleteFrame()&&!corrupt_tail.Closed());
    rejected=false;
    try{corrupt_tail.Receive();}catch(...){rejected=true;}
    assert(rejected&&corrupt_tail.Closed());close(pair[1]);

    // A sizer reporting an oversized future frame must be rejected before it
    // can be mistaken for a partial fragment and repeatedly select wait zero.
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    Stream oversized_tail(pair[0],[](std::span<const std::uint8_t> bytes)->std::size_t{
        if(bytes.empty())return 0;
        return bytes.front()==1?1:256*1024+1;
    });
    std::vector<std::uint8_t> oversized_batch(64,1);oversized_batch.push_back(2);
    assert(send(pair[1],oversized_batch.data(),oversized_batch.size(),0)==static_cast<ssize_t>(oversized_batch.size()));
    assert(oversized_tail.Receive().size()==64&&oversized_tail.HasCompleteFrame());
    rejected=false;
    try{oversized_tail.Receive();}catch(...){rejected=true;}
    assert(rejected&&oversized_tail.Closed());close(pair[1]);

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
