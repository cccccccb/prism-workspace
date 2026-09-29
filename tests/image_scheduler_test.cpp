#include "prism/runtime/image_resources.hpp"
#include "prism/runtime/task_scheduler.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <thread>

namespace {
using namespace prism::runtime;
using namespace std::chrono_literals;
constexpr std::uint64_t kMiB = 1024 * 1024;

class Gate {
public:
    void Enter()
    {
        std::unique_lock lock(mutex_);
        ++entered_;
        changed_.notify_all();
        changed_.wait(lock, [this] { return released_; });
    }

    void Await()
    {
        std::unique_lock lock(mutex_);
        assert(changed_.wait_for(lock, 5s, [this] { return entered_ != 0; }));
    }

    void Release()
    {
        std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    unsigned entered_{};
    bool released_{};
};

struct Inspector {
    ImageDescription description{1, 1, 4};
    std::atomic<unsigned> calls{};
    Gate *gate{};
    std::shared_ptr<SessionTaskBudget> budget;

    std::optional<ImageDescription> Inspect(const std::string &)
    {
        ++calls;
        if (budget) {
            const auto stats = budget->Stats();
            assert(stats.active >= 1 && stats.active <= 2 && stats.reserved_bytes >= 8192);
        }
        if (gate) {
            gate->Enter();
        }
        return description;
    }
};

struct Decoder {
    std::atomic<unsigned> calls{};
    Gate *gate{};
    std::shared_ptr<SessionTaskBudget> budget;
    std::thread::id owner{std::this_thread::get_id()};

    std::optional<DecodedImage> Decode(const std::string &, std::size_t limit)
    {
        assert(std::this_thread::get_id() != owner);
        assert(limit >= 4);
        ++calls;
        if (budget) {
            const auto stats = budget->Stats();
            assert(stats.active >= 1 && stats.active <= 2);
            assert(stats.reserved_bytes >= limit + 16 * kMiB);
        }
        if (gate) {
            gate->Enter();
        }
        return DecodedImage{1, 1, std::vector<std::uint8_t>(4, 255)};
    }
};

struct GenericOutput final : TaskOutput {
    std::uint64_t RetainedBytes() const noexcept override
    {
        return sizeof(GenericOutput);
    }
};

struct GenericWork {
    Gate *gate{};
    std::atomic<unsigned> *calls{};
    std::shared_ptr<SessionTaskBudget> budget;

    std::shared_ptr<const TaskOutput> operator()(std::stop_token) const
    {
        if (calls) {
            ++*calls;
        }
        if (budget) {
            const auto active = budget->Stats().active;
            assert(active >= 1 && active <= 2);
        }
        if (gate) {
            gate->Enter();
        }
        return std::make_shared<const GenericOutput>();
    }
};

std::shared_ptr<SessionTaskBudget> Budget()
{
    return SessionTaskBudget::Create({2, 64 * kMiB, 0});
}

void WaitReadable(int fd, std::chrono::steady_clock::time_point deadline)
{
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    assert(remaining.count() > 0);
    pollfd descriptor{fd, POLLIN, 0};
    assert(poll(&descriptor, 1, static_cast<int>(remaining.count())) == 1);
    assert(descriptor.revents == POLLIN);
}

void AwaitTerminal(ImageResources &resources, prism::contracts::ResourceId id)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (true) {
        resources.Poll();
        if (resources.State(id) != ImageState::Loading) {
            return;
        }
        WaitReadable(resources.CompletionFd(), deadline);
    }
}

TaskCompletion Take(TaskChannel &channel)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (true) {
        if (auto result = channel.TakeCompletion()) {
            return std::move(*result);
        }
        WaitReadable(channel.Fd(), deadline);
    }
}

void DrainCancelled(ImageResources &resources, const TaskScheduler &scheduler)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (true) {
        assert(resources.Poll().empty());
        if (!scheduler.Stats().outstanding) {
            break;
        }
        WaitReadable(resources.CompletionFd(), deadline);
    }
    assert(resources.Poll().empty());
}

void InspectionRejectsBeforeDecode()
{
    auto scheduler = std::make_shared<TaskScheduler>(Budget());
    Inspector inspector;
    Decoder decoder;
    ImageResources resources(std::bind_front(&Inspector::Inspect, &inspector),
                             std::bind_front(&Decoder::Decode, &decoder), scheduler, 4);
    inspector.description = {4097, 1, 4097 * 4};
    const auto oversized = resources.Request("over-dimension-limit");
    AwaitTerminal(resources, oversized);
    assert(resources.State(oversized) == ImageState::Failed);
    assert(decoder.calls == 0);

    inspector.description = {1, 1, 8};
    const auto inconsistent = resources.Request("invalid-inspection-byte-count");
    AwaitTerminal(resources, inconsistent);
    assert(resources.State(inconsistent) == ImageState::Failed);
    assert(decoder.calls == 0);

    inspector.description = {2, 1, 8};
    const auto cache_exceeded = resources.Request("over-cache-limit");
    AwaitTerminal(resources, cache_exceeded);
    assert(resources.State(cache_exceeded) == ImageState::Failed);
    assert(resources.DecodedBytes() == 0 && decoder.calls == 0);
    assert(inspector.calls == 3);
}

void DeduplicationAndRetainedOwnership()
{
    auto budget = Budget();
    auto scheduler = std::make_shared<TaskScheduler>(budget);
    Inspector inspector;
    inspector.budget = budget;
    Decoder decoder;
    decoder.budget = budget;
    ImageResources resources(std::bind_front(&Inspector::Inspect, &inspector),
                             std::bind_front(&Decoder::Decode, &decoder), scheduler, 4);
    const auto id = resources.Request("shared-image");
    assert(id && resources.Request("shared-image") == id);
    AwaitTerminal(resources, id);
    assert(resources.State(id) == ImageState::Ready && resources.Get(id));
    assert(resources.Request("shared-image") == id);
    assert(inspector.calls == 1 && decoder.calls == 1 && resources.DecodedBytes() == 4);
    const auto retained_bytes = budget->Stats().retained_bytes;
    assert(retained_bytes >= 4 && budget->Stats().active == 0);

    const auto no_space = resources.Request("second-image");
    AwaitTerminal(resources, no_space);
    assert(resources.State(no_space) == ImageState::Failed && decoder.calls == 1);
    assert(budget->Stats().retained_bytes == retained_bytes);

    auto renderer_owner = resources.Retain(id);
    auto other_owner = renderer_owner;
    assert(renderer_owner && renderer_owner.get() == resources.Get(id));
    assert(renderer_owner->rgba.size() == 4);
    resources.Release(id);
    assert(!resources.Get(id) && resources.DecodedBytes() == 0);
    assert(other_owner->width == 1 && other_owner->rgba.size() == 4);
    assert(budget->Stats().retained_bytes == retained_bytes);
    renderer_owner.reset();
    assert(budget->Stats().retained_bytes == retained_bytes);
    other_owner.reset();
    assert(budget->Stats().reserved_bytes == 0);
}

void SharedWorkersAndPreallocationBudget()
{
    auto budget = Budget();
    auto scheduler = std::make_shared<TaskScheduler>(budget);
    auto graph = scheduler->OpenChannel();
    Inspector inspector;
    inspector.budget = budget;
    Gate decoding;
    Decoder decoder;
    decoder.budget = budget;
    decoder.gate = &decoding;
    ImageResources resources(std::bind_front(&Inspector::Inspect, &inspector),
                             std::bind_front(&Decoder::Decode, &decoder), scheduler);
    const auto id = resources.Request("parallel-resource");
    WaitReadable(resources.CompletionFd(), std::chrono::steady_clock::now() + 5s);
    resources.Poll();
    decoding.Await();

    Gate preparing;
    assert(graph->Submit({1, TaskPriority::Critical, 1024, GenericWork{&preparing, {}, budget}}) ==
           TaskSubmitResult::Accepted);
    preparing.Await();
    assert(scheduler->Stats().workers == 2 && scheduler->Stats().active == 2);
    assert(budget->Stats().active == 2);
    std::atomic<unsigned> third_calls{};
    assert(graph->Submit({2, TaskPriority::Deferred, 1024,
                          GenericWork{{}, &third_calls, budget}}) == TaskSubmitResult::Accepted);
    assert(third_calls == 0);

    decoding.Release();
    AwaitTerminal(resources, id);
    assert(resources.State(id) == ImageState::Ready && decoder.calls == 1);
    preparing.Release();
    const auto first = Take(*graph);
    const auto second = Take(*graph);
    assert(first.output && second.output && first.id != second.id && third_calls == 1);
    assert(budget->Stats().active == 0);
}

void LateInspectionAndDecodeAreDiscarded()
{
    auto budget = Budget();
    auto scheduler = std::make_shared<TaskScheduler>(budget);
    Gate inspecting;
    Inspector inspector;
    inspector.gate = &inspecting;
    Decoder decoder;
    ImageResources resources(std::bind_front(&Inspector::Inspect, &inspector),
                             std::bind_front(&Decoder::Decode, &decoder), scheduler);
    const auto inspect_id = resources.Request("cancel-inspection");
    inspecting.Await();
    resources.Release(inspect_id);
    inspecting.Release();
    DrainCancelled(resources, *scheduler);
    assert(inspector.calls == 1 && decoder.calls == 0);
    assert(resources.State(inspect_id) == ImageState::Failed && !resources.Retain(inspect_id));
    assert(budget->Stats().reserved_bytes == 0);

    inspector.gate = nullptr;
    Gate decoding;
    decoder.gate = &decoding;
    const auto decode_id = resources.Request("cancel-decode");
    WaitReadable(resources.CompletionFd(), std::chrono::steady_clock::now() + 5s);
    resources.Poll();
    decoding.Await();
    resources.Release(decode_id);
    decoding.Release();
    DrainCancelled(resources, *scheduler);
    assert(decoder.calls == 1 && resources.DecodedBytes() == 0);
    assert(resources.State(decode_id) == ImageState::Failed && !resources.Get(decode_id));
    assert(budget->Stats().reserved_bytes == 0);
}

void CancelQueuedInspection()
{
    auto budget = Budget();
    auto scheduler = std::make_shared<TaskScheduler>(budget, 1);
    auto graph = scheduler->OpenChannel();
    Gate preparing;
    assert(graph->Submit({1, TaskPriority::Critical, 1024, GenericWork{&preparing, {}, budget}}) ==
           TaskSubmitResult::Accepted);
    preparing.Await();
    Inspector inspector;
    Decoder decoder;
    ImageResources resources(std::bind_front(&Inspector::Inspect, &inspector),
                             std::bind_front(&Decoder::Decode, &decoder), scheduler);
    const auto id = resources.Request("queued-inspection");
    assert(id && resources.State(id) == ImageState::Loading);
    resources.Release(id);
    preparing.Release();
    auto graph_result = Take(*graph);
    graph_result.output.reset();
    DrainCancelled(resources, *scheduler);
    assert(inspector.calls == 0 && decoder.calls == 0);
    assert(budget->Stats().reserved_bytes == 0);
}

void BusyUsesCapacityWake()
{
    auto scheduler = std::make_shared<TaskScheduler>(Budget(), 1, 1);
    auto graph = scheduler->OpenChannel();
    Gate preparing;
    assert(graph->Submit({1, TaskPriority::Critical, 1024, GenericWork{&preparing}}) ==
           TaskSubmitResult::Accepted);
    preparing.Await();
    Inspector inspector;
    Decoder decoder;
    ImageResources resources(std::bind_front(&Inspector::Inspect, &inspector),
                             std::bind_front(&Decoder::Decode, &decoder), scheduler);
    const auto id = resources.Request("waiting-for-capacity");
    assert(id && resources.State(id) == ImageState::Loading);
    assert(resources.Poll().empty() && inspector.calls == 0 && decoder.calls == 0);
    pollfd ready{resources.CompletionFd(), POLLIN, 0};
    assert(poll(&ready, 1, 0) == 0);

    preparing.Release();
    assert(Take(*graph).output);
    WaitReadable(resources.CompletionFd(), std::chrono::steady_clock::now() + 5s);
    resources.Poll(); // Capacity progress dispatches the previously unqueued inspector.
    AwaitTerminal(resources, id);
    assert(resources.State(id) == ImageState::Ready && resources.Get(id));
    assert(inspector.calls == 1 && decoder.calls == 1);
    assert(resources.Poll().empty());
    assert(poll(&ready, 1, 0) == 0);
}

void EmptyLegacyDecoderRejected()
{
    bool rejected = false;
    try {
        ImageResources resources(ImageResources::Decoder{});
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
}
} // namespace

int main()
{
    InspectionRejectsBeforeDecode();
    DeduplicationAndRetainedOwnership();
    SharedWorkersAndPreallocationBudget();
    LateInspectionAndDecodeAreDiscarded();
    CancelQueuedInspection();
    BusyUsesCapacityWake();
    EmptyLegacyDecoderRejected();
    std::cout << "image_scheduler_test: passed\n";
}
