#include "prism/runtime/prepared_component.hpp"
#include "prism/runtime/task_scheduler.hpp"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <thread>
#include <vector>

namespace {
using namespace prism::runtime;
using namespace std::chrono_literals;

struct ValueOutput : TaskOutput {
    ValueOutput(std::uint64_t number, std::uint64_t bytes) : value(number), retained(bytes)
    {
    }

    std::uint64_t RetainedBytes() const noexcept override
    {
        return retained;
    }

    std::uint64_t value;
    std::uint64_t retained;
};

struct ValueWork {
    std::uint64_t value;
    std::uint64_t retained{32};

    std::shared_ptr<const TaskOutput> operator()(std::stop_token) const
    {
        return std::make_shared<const ValueOutput>(value, retained);
    }
};

class Barrier {
public:
    std::shared_ptr<const TaskOutput> Work(std::uint64_t id, std::stop_token stop)
    {
        std::unique_lock lock(mutex_);
        entered_.push_back(id);
        worker_ = std::this_thread::get_id();
        changed_.notify_all();
        if (!changed_.wait(lock, stop, [this] { return released_; })) {
            return {};
        }
        return std::make_shared<const ValueOutput>(id, 32);
    }

    void Await(std::size_t count)
    {
        std::unique_lock lock(mutex_);
        assert(changed_.wait_for(lock, 5s, [this, count] { return entered_.size() >= count; }));
        assert(worker_ != std::this_thread::get_id());
    }

    void Release()
    {
        std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::vector<std::uint64_t> entered_;
    std::thread::id worker_;
    bool released_{};
};

TaskCompletion Take(TaskChannel &channel)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (true) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        assert(remaining.count() > 0);
        pollfd ready{channel.Fd(), POLLIN, 0};
        assert(poll(&ready, 1, static_cast<int>(remaining.count())) == 1 &&
               ready.revents == POLLIN);
        if (auto result = channel.TakeCompletion()) {
            return std::move(*result);
        }
    }
}

std::shared_ptr<SessionTaskBudget> Budget(std::uint32_t active = 2, std::uint64_t memory = 65536)
{
    return SessionTaskBudget::Create({active, memory, 0});
}

void ParallelismAndPriority()
{
    auto budget = Budget();
    TaskScheduler scheduler(budget);
    auto channel = scheduler.OpenChannel();
    assert(scheduler.Stats().workers == 0);
    Barrier barrier;
    assert(channel->Submit(
               {1, TaskPriority::Critical, 64, std::bind_front(&Barrier::Work, &barrier, 1)}) ==
           TaskSubmitResult::Accepted);
    assert(channel->Submit(
               {2, TaskPriority::Resource, 64, std::bind_front(&Barrier::Work, &barrier, 2)}) ==
           TaskSubmitResult::Accepted);
    barrier.Await(2);
    assert(scheduler.Stats().workers == 2 && scheduler.Stats().active == 2);
    assert(budget->Stats().active == 2);
    barrier.Release();
    auto first = Take(*channel);
    auto second = Take(*channel);
    assert(first.output && second.output && first.id != second.id);

    TaskScheduler serial(Budget(), 1);
    auto ordered = serial.OpenChannel();
    Barrier gate;
    assert(ordered->Submit(
               {10, TaskPriority::Critical, 64, std::bind_front(&Barrier::Work, &gate, 10)}) ==
           TaskSubmitResult::Accepted);
    gate.Await(1);
    assert(ordered->Submit({11, TaskPriority::Deferred, 64, ValueWork{11}}) ==
           TaskSubmitResult::Accepted);
    assert(ordered->Submit({12, TaskPriority::Resource, 64, ValueWork{12}}) ==
           TaskSubmitResult::Accepted);
    assert(ordered->Submit({13, TaskPriority::Critical, 64, ValueWork{13}}) ==
           TaskSubmitResult::Accepted);
    gate.Release();
    assert(Take(*ordered).id == 10);
    assert(Take(*ordered).id == 13);
    assert(Take(*ordered).id == 12);
    assert(Take(*ordered).id == 11);
}

void BoundsAndCancellation()
{
    TaskScheduler scheduler(Budget(), 2, 2);
    auto channel = scheduler.OpenChannel(2, 1);
    Barrier barrier;
    assert(channel->Submit(
               {1, TaskPriority::Critical, 64, std::bind_front(&Barrier::Work, &barrier, 1)}) ==
           TaskSubmitResult::Accepted);
    barrier.Await(1);
    assert(channel->Submit({2, TaskPriority::Critical, 64, ValueWork{2}}) ==
           TaskSubmitResult::Accepted);
    assert(channel->Submit({3, TaskPriority::Critical, 64, ValueWork{3}}) ==
           TaskSubmitResult::Busy);
    assert(channel->Submit({2, TaskPriority::Critical, 64, ValueWork{2}}) ==
           TaskSubmitResult::Invalid);
    channel->Cancel(2);
    channel->Cancel(1);
    auto first = Take(*channel);
    auto second = Take(*channel);
    assert(first.error && first.error->code == TaskErrorCode::Cancelled);
    assert(second.error && second.error->code == TaskErrorCode::Cancelled);
    assert(scheduler.Stats().outstanding == 0);
    assert(channel->Submit({3, TaskPriority::Critical, 64, ValueWork{3}}) ==
           TaskSubmitResult::Accepted);
    auto finished = Take(*channel);
    assert(finished.output);
    assert(channel->Submit({4, TaskPriority::Critical, 64, ValueWork{4}}) ==
           TaskSubmitResult::Accepted);
    pollfd ready{channel->Fd(), POLLIN, 0};
    assert(poll(&ready, 1, 5000) == 1);
    channel->Cancel(4);
    auto cancelled = Take(*channel);
    assert(cancelled.error && cancelled.error->code == TaskErrorCode::Cancelled);
    assert(!cancelled.output);
}

void RetentionAndBudgetErrors()
{
    auto budget = Budget();
    TaskScheduler scheduler(budget);
    auto channel = scheduler.OpenChannel();
    assert(channel->Submit({1, TaskPriority::Critical, 64, ValueWork{1}}) ==
           TaskSubmitResult::Accepted);
    auto result = Take(*channel);
    assert(result.output && !result.error);
    assert(budget->Stats().active == 0 && budget->Stats().retained_bytes == 32);
    auto output_copy = result.output;
    result.output.reset();
    assert(budget->Stats().retained_bytes == 32);
    output_copy.reset();
    assert(budget->Stats().reserved_bytes == 0);

    assert(channel->Submit({2, TaskPriority::Critical, 64, ValueWork{2, 65}}) ==
           TaskSubmitResult::Accepted);
    auto oversized = Take(*channel);
    assert(oversized.error && oversized.error->code == TaskErrorCode::Budget && !oversized.output);
    assert(budget->Stats().reserved_bytes == 0);
    assert(channel->Submit({3, TaskPriority::Critical, 65537, ValueWork{3}}) ==
           TaskSubmitResult::Accepted);
    assert(Take(*channel).error->code == TaskErrorCode::Budget);

    auto held = budget->Acquire(65536, TaskPriority::Critical);
    assert(channel->Submit({4, TaskPriority::Critical, 64, ValueWork{4}}) ==
           TaskSubmitResult::Accepted);
    channel->Stop();
    held.reset();
    assert(budget->Stats().reserved_bytes == 0);
    assert(channel->Submit({5, TaskPriority::Critical, 64, ValueWork{5}}) ==
           TaskSubmitResult::Closed);
}

void CapacityProgressWakesEmptyChannel()
{
    TaskScheduler scheduler(Budget(), 1, 1);
    auto occupied = scheduler.OpenChannel();
    auto waiting = scheduler.OpenChannel();
    Barrier barrier;
    assert(occupied->Submit(
               {1, TaskPriority::Critical, 64, std::bind_front(&Barrier::Work, &barrier, 1)}) ==
           TaskSubmitResult::Accepted);
    barrier.Await(1);
    assert(waiting->Submit({1, TaskPriority::Resource, 64, ValueWork{9}}) ==
           TaskSubmitResult::Busy);
    pollfd ready{waiting->Fd(), POLLIN, 0};
    assert(poll(&ready, 1, 0) == 0);

    barrier.Release();
    assert(Take(*occupied).output);
    assert(poll(&ready, 1, 5000) == 1 && ready.revents == POLLIN);
    assert(!waiting->TakeCompletion());
    assert(poll(&ready, 1, 0) == 0);
    assert(waiting->Submit({1, TaskPriority::Resource, 64, ValueWork{9}}) ==
           TaskSubmitResult::Accepted);
    assert(std::dynamic_pointer_cast<const ValueOutput>(Take(*waiting).output)->value == 9);
    assert(!waiting->TakeCompletion());
    assert(poll(&ready, 1, 0) == 0);
}

void CompletionOnlyChannelStaysQuiet()
{
    TaskScheduler scheduler(Budget(), 1, 1);
    auto other = scheduler.OpenChannel();
    auto completion_only = scheduler.OpenChannel(2, 1, false);
    assert(other->Submit({1, TaskPriority::Critical, 64, ValueWork{1}}) ==
           TaskSubmitResult::Accepted);
    assert(completion_only->Submit({1, TaskPriority::Resource, 64, ValueWork{2}}) ==
           TaskSubmitResult::Busy);
    assert(Take(*other).output);
    pollfd ready{completion_only->Fd(), POLLIN, 0};
    assert(poll(&ready, 1, 0) == 0);
    assert(!completion_only->TakeCompletion());

    assert(completion_only->Submit({1, TaskPriority::Resource, 64, ValueWork{2}}) ==
           TaskSubmitResult::Accepted);
    assert(poll(&ready, 1, 5000) == 1 && ready.revents == POLLIN);
    const auto result = completion_only->TakeCompletion();
    assert(result && result->output);
    assert(poll(&ready, 1, 0) == 0);
}

void ChannelIsolationAndShutdown()
{
    auto budget = Budget();
    TaskScheduler scheduler(budget);
    auto first = scheduler.OpenChannel();
    auto second = scheduler.OpenChannel();
    Barrier barrier;
    assert(first->Submit(
               {1, TaskPriority::Critical, 64, std::bind_front(&Barrier::Work, &barrier, 1)}) ==
           TaskSubmitResult::Accepted);
    barrier.Await(1);
    assert(second->Submit({1, TaskPriority::Resource, 64, ValueWork{9}}) ==
           TaskSubmitResult::Accepted);
    first->Stop();
    assert(!first->TakeCompletion());
    const auto result = Take(*second);
    assert(result.output &&
           std::dynamic_pointer_cast<const ValueOutput>(result.output)->value == 9);
    assert(scheduler.Stats().outstanding == 0);
    scheduler.Stop();
    scheduler.Stop();
    assert(second->Submit({2, TaskPriority::Critical, 64, ValueWork{2}}) ==
           TaskSubmitResult::Closed);
}

void PreparedRetention()
{
    auto budget = Budget();
    TaskScheduler scheduler(budget);
    auto channel = scheduler.OpenChannel();
    assert(channel->Submit({1, TaskPriority::Critical, 64, ValueWork{1}}) ==
           TaskSubmitResult::Accepted);
    auto output = Take(*channel).output;
    auto prepared = PrepareComponent("Text(\"retained\")");
    assert(prepared.RetainedBytes() > 0);
    {
        auto retained = prepared.WithRetention(output);
        output.reset();
        assert(budget->Stats().retained_bytes == 32);
        auto copied = retained;
        assert(copied.Root().kind == Kind::Text);
    }
    assert(budget->Stats().reserved_bytes == 0);
}
} // namespace

int main()
{
    ParallelismAndPriority();
    BoundsAndCancellation();
    RetentionAndBudgetErrors();
    CapacityProgressWakesEmptyChannel();
    CompletionOnlyChannelStaysQuiet();
    ChannelIsolationAndShutdown();
    PreparedRetention();
    std::cout << "task_scheduler_test: passed\n";
}
