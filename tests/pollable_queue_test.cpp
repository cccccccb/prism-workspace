#include "prism/runtime/pollable_queue.hpp"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <variant>

namespace {

using prism::runtime::PollableQueue;
using prism::runtime::QueuePushResult;

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

void WakeAndClose()
{
    PollableQueue<int> queue(2);
    assert(!Ready(queue.Fd()));
    assert(queue.TryPush(1) == QueuePushResult::Accepted);
    assert(Ready(queue.Fd()));
    assert(queue.TryPush(2) == QueuePushResult::Accepted);
    assert(queue.TryPush(3) == QueuePushResult::Busy);
    assert(Ready(queue.Fd()));

    assert(queue.TryPop() == 1);
    assert(Ready(queue.Fd()));
    assert(queue.TryPop() == 2);
    assert(!Ready(queue.Fd()));
    assert(!queue.TryPop());

    assert(queue.Close());
    assert(queue.IsClosed());
    assert(Ready(queue.Fd()));
    assert(!queue.TryPop());
    assert(queue.TryPush(4) == QueuePushResult::Closed);
    assert(queue.Close());
    assert(Ready(queue.Fd()));

    PollableQueue<int> pending(1);
    assert(pending.TryPush(8) == QueuePushResult::Accepted);
    assert(pending.Close());
    assert(pending.TryPop() == 8);
    assert(Ready(pending.Fd()));
}

void InvalidCapacity()
{
    bool rejected = false;
    try {
        PollableQueue<int> queue(0);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
}

void RejectedMovesStayOwned()
{
    PollableQueue<std::unique_ptr<int>> queue(1);
    auto first = std::make_unique<int>(1);
    auto second = std::make_unique<int>(2);

    assert(queue.TryPush(std::move(first)) == QueuePushResult::Accepted);
    assert(!first);
    assert(queue.TryPush(std::move(second)) == QueuePushResult::Busy);
    assert(second && *second == 2);
    assert(**queue.TryPop() == 1);
    assert(queue.TryPush(std::move(second)) == QueuePushResult::Accepted);
    assert(!second);

    assert(queue.Close());
    auto rejected = std::make_unique<int>(3);
    assert(queue.TryPush(std::move(rejected)) == QueuePushResult::Closed);
    assert(rejected && *rejected == 3);
}

void SignalFailureDoesNotPublish()
{
    PollableQueue<std::unique_ptr<int>> queue(1);
    auto value = std::make_unique<int>(9);

    // Fault injection: this test owns the queue and deliberately invalidates
    // its descriptor before publishing. Applications must not close Fd().
    assert(close(queue.Fd()) == 0);
    assert(queue.TryPush(std::move(value)) == QueuePushResult::SignalError);
    assert(value && *value == 9);
    assert(queue.Size() == 0);
    assert(!queue.Close());
    assert(!queue.IsClosed());
}

struct Frame {
    int number{};
    bool required{};
};

struct Control {
    int number{};
};

using Command = std::variant<Frame, Control>;

bool ReplaceOrdinaryFrame(const Command &old_command, const Command &new_command) noexcept
{
    const auto *old_frame = std::get_if<Frame>(&old_command);
    const auto *new_frame = std::get_if<Frame>(&new_command);
    return old_frame && new_frame && !old_frame->required && !new_frame->required;
}

void TailReplacementPreservesBarriers()
{
    PollableQueue<Command> queue(3);
    assert(queue.TryPush(Frame{1, false}) == QueuePushResult::Accepted);
    assert(queue.TryPush(Control{2}) == QueuePushResult::Accepted);
    assert(queue.TryPushLatest(Frame{3, false}, ReplaceOrdinaryFrame) == QueuePushResult::Accepted);
    assert(queue.TryPushLatest(Frame{4, false}, ReplaceOrdinaryFrame) == QueuePushResult::Replaced);
    assert(queue.Size() == 3);

    auto first = queue.TryPop();
    assert(first && std::get<Frame>(*first).number == 1);
    auto second = queue.TryPop();
    assert(second && std::get<Control>(*second).number == 2);
    auto third = queue.TryPop();
    assert(third && std::get<Frame>(*third).number == 4);
    assert(!Ready(queue.Fd()));

    assert(queue.TryPush(Frame{5, true}) == QueuePushResult::Accepted);
    assert(queue.TryPushLatest(Frame{6, false}, ReplaceOrdinaryFrame) == QueuePushResult::Accepted);
    assert(queue.TryPushLatest(Frame{7, true}, ReplaceOrdinaryFrame) == QueuePushResult::Accepted);
    assert(queue.TryPushLatest(Frame{8, false}, ReplaceOrdinaryFrame) == QueuePushResult::Busy);
    assert(std::get<Frame>(*queue.TryPop()).number == 5);
    assert(std::get<Frame>(*queue.TryPop()).number == 6);
    assert(std::get<Frame>(*queue.TryPop()).number == 7);
}

struct Producer {
    PollableQueue<int> &queue;
    std::atomic<bool> &failed;
    int total;

    void operator()()
    {
        for (int number = 0; number < total;) {
            int candidate = number;
            const auto result = queue.TryPush(std::move(candidate));
            if (result == QueuePushResult::Accepted) {
                ++number;
            } else if (result == QueuePushResult::Busy) {
                std::this_thread::yield();
            } else {
                failed = true;
                return;
            }
        }
    }
};

void ConcurrentWakeups()
{
    constexpr int total = 5000;
    PollableQueue<int> queue(16);
    std::atomic<bool> producer_failed{false};
    std::thread producer(Producer{queue, producer_failed, total});

    int received = 0;
    bool ordered = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (received < total && std::chrono::steady_clock::now() < deadline) {
        if (!Ready(queue.Fd(), 100)) {
            continue;
        }

        const auto item = queue.TryPop();
        if (item) {
            ordered = ordered && *item == received;
            ++received;
        }
    }

    assert(queue.Close());
    producer.join();
    assert(!producer_failed);
    assert(received == total);
    assert(ordered);
}

} // namespace

int main()
{
    InvalidCapacity();
    RejectedMovesStayOwned();
    SignalFailureDoesNotPublish();
    WakeAndClose();
    TailReplacementPreservesBarriers();
    ConcurrentWakeups();
}
