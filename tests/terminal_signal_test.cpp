#include "prism/runtime/pollable_queue.hpp"
#include "prism/runtime/terminal_signal.hpp"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using prism::runtime::PollableQueue;
using prism::runtime::QueuePushResult;
using prism::runtime::TerminalReason;
using prism::runtime::TerminalSignal;

bool Ready(int fd, int timeout_ms = 0)
{
    pollfd descriptor{fd, POLLIN, 0};
    int result;
    do {
        result = poll(&descriptor, 1, timeout_ms);
    } while (result < 0 && errno == EINTR);

    assert(result >= 0);
    assert((descriptor.revents & (POLLERR | POLLNVAL)) == 0);
    return result == 1 && (descriptor.revents & POLLIN) != 0;
}

void FirstFailurePersists()
{
    TerminalSignal signal;
    assert(signal.Reason() == TerminalReason::None);
    assert(!signal.StopRequested());
    assert(!Ready(signal.Fd()));
    assert(!signal.Fail(TerminalReason::None));
    assert(!Ready(signal.Fd()));

    assert(signal.Fail(TerminalReason::EventQueueFailure));
    assert(signal.Reason() == TerminalReason::EventQueueFailure);
    assert(Ready(signal.Fd()));
    assert(!signal.Fail(TerminalReason::RenderFailure));
    assert(signal.Reason() == TerminalReason::EventQueueFailure);
    assert(Ready(signal.Fd()));
    assert(!signal.NotificationFailed());
}

void StopAndFailureAreIndependent()
{
    TerminalSignal signal;
    assert(signal.RequestStop());
    assert(signal.StopRequested());
    assert(signal.Reason() == TerminalReason::None);
    assert(Ready(signal.Fd()));
    assert(!signal.RequestStop());

    assert(signal.Fail(TerminalReason::PlatformFailure));
    assert(signal.Reason() == TerminalReason::PlatformFailure);
    assert(Ready(signal.Fd()));
}

void FullQueueCannotHideFailure()
{
    PollableQueue<int> queue(1);
    TerminalSignal signal;
    assert(queue.TryPush(1) == QueuePushResult::Accepted);
    assert(queue.TryPush(2) == QueuePushResult::Busy);

    assert(signal.Fail(TerminalReason::EventQueueFailure));
    assert(signal.RequestStop());
    assert(Ready(signal.Fd()));
    assert(signal.Reason() == TerminalReason::EventQueueFailure);
    assert(signal.StopRequested());
    assert(queue.Size() == 1);
}

struct ConcurrentProducer {
    TerminalSignal &signal;
    std::atomic<bool> &start;
    std::atomic<int> &first_failures;
    std::atomic<int> &first_stops;
    int index;

    void operator()() const
    {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        if (signal.Fail(index % 2 ? TerminalReason::RenderFailure
                                  : TerminalReason::PlatformFailure)) {
            ++first_failures;
        }
        if (signal.RequestStop()) {
            ++first_stops;
        }
    }
};

struct ConcurrentWaiter {
    TerminalSignal &signal;
    std::atomic<bool> &awakened;

    void operator()() const
    {
        awakened.store(Ready(signal.Fd(), 2000));
    }
};

void ConcurrentPublicationWakesWaiter()
{
    TerminalSignal signal;
    std::atomic<bool> start{};
    std::atomic<int> first_failures{};
    std::atomic<int> first_stops{};
    std::vector<std::thread> producers;
    for (int index = 0; index < 8; ++index) {
        producers.emplace_back(
            ConcurrentProducer{signal, start, first_failures, first_stops, index});
    }

    std::atomic<bool> awakened{};
    std::thread waiter(ConcurrentWaiter{signal, awakened});
    start.store(true, std::memory_order_release);
    for (auto &producer : producers) {
        producer.join();
    }
    waiter.join();

    assert(awakened);
    assert(first_failures == 1);
    assert(first_stops == 1);
    assert(signal.StopRequested());
    assert(signal.Reason() == TerminalReason::RenderFailure ||
           signal.Reason() == TerminalReason::PlatformFailure);
    assert(Ready(signal.Fd()));
    assert(!signal.NotificationFailed());
}

void NotificationErrorKeepsReason()
{
    TerminalSignal signal;
    // Fault injection: only this test owns the signal. Applications must not
    // close Fd(); the owner must use bounded polling if notification fails.
    assert(close(signal.Fd()) == 0);
    assert(signal.Fail(TerminalReason::InternalFailure));
    assert(signal.RequestStop());
    assert(signal.Reason() == TerminalReason::InternalFailure);
    assert(signal.StopRequested());
    assert(signal.NotificationFailed());
}

} // namespace

int main()
{
    FirstFailurePersists();
    StopAndFailureAreIndependent();
    FullQueueCannotHideFailure();
    ConcurrentPublicationWakesWaiter();
    NotificationErrorKeepsReason();
}
