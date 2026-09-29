#include "prism/runtime/pollable_queue.hpp"
#include "prism/runtime/render_worker_lifecycle.hpp"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <poll.h>
#include <thread>

namespace {

using namespace std::chrono_literals;
using prism::runtime::PollableQueue;
using prism::runtime::QueuePushResult;
using prism::runtime::RenderWorkerCloseWait;
using prism::runtime::RenderWorkerGeneration;
using prism::runtime::RenderWorkerLifecycle;
using prism::runtime::RenderWorkerOpenOutcome;
using prism::runtime::RenderWorkerOpenStatus;
using prism::runtime::RenderWorkerOpenWait;
using prism::runtime::TerminalReason;
using prism::runtime::TerminalSignal;

bool Ready(int fd, int timeout_ms)
{
    pollfd descriptor{fd, POLLIN, 0};
    int result;
    do {
        result = poll(&descriptor, 1, timeout_ms);
    } while (result < 0 && errno == EINTR);

    assert(result >= 0);
    assert(!(descriptor.revents & (POLLERR | POLLNVAL)));
    return result == 1 && (descriptor.revents & POLLIN);
}

void GenerationAndOpenResults()
{
    TerminalSignal first_terminal;
    RenderWorkerLifecycle first(first_terminal);
    const auto first_generation = first.BeginOpen();
    assert(first_generation);
    assert(first.OpenStatus(*first_generation) == RenderWorkerOpenStatus::Started);
    assert(!first.BeginOpen());
    assert(first.WaitOpen(*first_generation, 0ms) == RenderWorkerOpenWait::TimedOut);

    TerminalSignal second_terminal;
    RenderWorkerLifecycle second(second_terminal);
    const auto second_generation = second.BeginOpen();
    assert(second_generation);
    assert(*first_generation != *second_generation);
    assert(!first.CompleteOpen(*second_generation, RenderWorkerOpenOutcome::Opened));
    assert(first.OpenStatus(*second_generation) == RenderWorkerOpenStatus::Stale);
    assert(first.WaitOpen(*second_generation, 0ms) == RenderWorkerOpenWait::Stale);
    assert(first.WaitClose(*second_generation, 0ms) == RenderWorkerCloseWait::Stale);
    assert(!first.RequestClose(*second_generation));
    assert(!first.CompleteClose(*second_generation));
    assert(first_terminal.Reason() == TerminalReason::None);

    assert(first.CompleteOpen(*first_generation, RenderWorkerOpenOutcome::Opened));
    assert(first.WaitOpen(*first_generation, 0ms) == RenderWorkerOpenWait::Opened);
    assert(!first.CompleteOpen(*first_generation, RenderWorkerOpenOutcome::Failed));
    assert(first_terminal.Reason() == TerminalReason::None);
    assert(first.RequestClose(*first_generation));
    assert(first.WaitClose(*first_generation, 0ms) == RenderWorkerCloseWait::TimedOut);
    assert(first.CompleteClose(*first_generation));
    assert(first.WaitClose(*first_generation, 0ms) == RenderWorkerCloseWait::Acknowledged);
    assert(!first.CompleteClose(*first_generation));
}

void OpenFailureDoesNotAcknowledgeClose()
{
    TerminalSignal terminal;
    RenderWorkerLifecycle lifecycle(terminal);
    const auto generation = lifecycle.BeginOpen();
    assert(generation);

    assert(lifecycle.CompleteOpen(*generation, RenderWorkerOpenOutcome::Failed));
    assert(lifecycle.WaitOpen(*generation, 0ms) == RenderWorkerOpenWait::Failed);
    assert(terminal.Reason() == TerminalReason::OpenFailure);
    assert(Ready(terminal.Fd(), 0));
    assert(lifecycle.WaitClose(*generation, 0ms) == RenderWorkerCloseWait::TimedOut);

    assert(lifecycle.RequestClose(*generation));
    assert(lifecycle.CloseRequested(*generation));
    assert(lifecycle.WaitClose(*generation, 0ms) == RenderWorkerCloseWait::TimedOut);
    assert(lifecycle.CompleteClose(*generation));
    assert(lifecycle.WaitClose(*generation, 0ms) == RenderWorkerCloseWait::Acknowledged);
}

struct OpenWaiter {
    RenderWorkerLifecycle &lifecycle;
    RenderWorkerGeneration generation;
    std::atomic<RenderWorkerOpenWait> &result;

    void operator()() const
    {
        result.store(lifecycle.WaitOpen(generation, 2s), std::memory_order_release);
    }
};

void ClosingPendingOpenCancelsWaiter()
{
    TerminalSignal terminal;
    RenderWorkerLifecycle lifecycle(terminal);
    const auto generation = lifecycle.BeginOpen();
    assert(generation);
    std::atomic<RenderWorkerOpenWait> result{RenderWorkerOpenWait::TimedOut};
    std::thread waiter(OpenWaiter{lifecycle, *generation, result});

    assert(lifecycle.RequestClose(*generation));
    waiter.join();
    assert(result.load(std::memory_order_acquire) == RenderWorkerOpenWait::Cancelled);
    assert(lifecycle.OpenStatus(*generation) == RenderWorkerOpenStatus::Cancelled);
    assert(lifecycle.CloseRequested(*generation));
    assert(Ready(terminal.Fd(), 0));
    assert(!lifecycle.CompleteOpen(*generation, RenderWorkerOpenOutcome::Opened));
    assert(!lifecycle.RequestClose(*generation));
    assert(lifecycle.WaitClose(*generation, 0ms) == RenderWorkerCloseWait::TimedOut);
    assert(lifecycle.CompleteClose(*generation));
    assert(lifecycle.WaitClose(*generation, 0ms) == RenderWorkerCloseWait::Acknowledged);
}

struct WorkerAfterClose {
    RenderWorkerLifecycle &lifecycle;
    RenderWorkerGeneration generation;
    TerminalSignal &terminal;
    std::mutex &mutex;
    std::condition_variable &cleanup_gate;
    bool &cleanup_complete;
    std::atomic<bool> &woke;

    void operator()() const
    {
        woke.store(Ready(terminal.Fd(), 2000), std::memory_order_release);
        if (!lifecycle.CloseRequested(generation)) {
            return;
        }

        std::unique_lock lock(mutex);
        cleanup_gate.wait(lock, [this] { return cleanup_complete; });
        lock.unlock();

        lifecycle.CompleteClose(generation);
    }
};

void FullCommandQueueCannotHideCloseOrForgeAck()
{
    PollableQueue<int> commands(1);
    assert(commands.TryPush(1) == QueuePushResult::Accepted);
    assert(commands.TryPush(2) == QueuePushResult::Busy);

    TerminalSignal terminal;
    RenderWorkerLifecycle lifecycle(terminal);
    const auto generation = lifecycle.BeginOpen();
    assert(generation);
    assert(lifecycle.CompleteOpen(*generation, RenderWorkerOpenOutcome::Opened));

    std::mutex mutex;
    std::condition_variable cleanup_gate;
    bool cleanup_complete{};
    std::atomic<bool> woke{};
    std::thread worker(WorkerAfterClose{lifecycle, *generation, terminal, mutex, cleanup_gate,
                                        cleanup_complete, woke});

    assert(lifecycle.RequestClose(*generation));
    assert(lifecycle.WaitClose(*generation, 20ms) == RenderWorkerCloseWait::TimedOut);
    assert(commands.Size() == 1);
    assert(Ready(terminal.Fd(), 0));

    {
        std::lock_guard lock(mutex);
        cleanup_complete = true;
    }
    cleanup_gate.notify_one();
    assert(lifecycle.WaitClose(*generation, 2s) == RenderWorkerCloseWait::Acknowledged);
    worker.join();
    assert(woke.load(std::memory_order_acquire));
    assert(terminal.StopRequested());
}

void WorkerMayCancelBeforeOpenCompletes()
{
    TerminalSignal terminal;
    RenderWorkerLifecycle lifecycle(terminal);
    const auto generation = lifecycle.BeginOpen();
    assert(generation);

    assert(lifecycle.CompleteOpen(*generation, RenderWorkerOpenOutcome::Cancelled));
    assert(lifecycle.WaitOpen(*generation, 0ms) == RenderWorkerOpenWait::Cancelled);
    assert(lifecycle.CloseRequested(*generation));
    assert(terminal.StopRequested());
    assert(lifecycle.WaitClose(*generation, 0ms) == RenderWorkerCloseWait::TimedOut);
    assert(lifecycle.CompleteClose(*generation));
    assert(lifecycle.WaitClose(*generation, 0ms) == RenderWorkerCloseWait::Acknowledged);
}

} // namespace

int main()
{
    GenerationAndOpenResults();
    OpenFailureDoesNotAcknowledgeClose();
    ClosingPendingOpenCancelsWaiter();
    FullCommandQueueCannotHideCloseOrForgeAck();
    WorkerMayCancelBeforeOpenCompletes();
}
