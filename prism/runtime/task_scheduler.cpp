#include "prism/runtime/task_scheduler.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <sys/eventfd.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace prism::runtime {
namespace {
TaskError Cancellation()
{
    return {TaskErrorCode::Cancelled, "Task cancelled"};
}
} // namespace

struct TaskScheduler::Impl {
    struct Job;

    struct Channel {
        Channel(std::size_t limit, std::size_t concurrency, bool capacity_progress)
            : outstanding_limit(limit), active_limit(concurrency), watch_capacity(capacity_progress)
        {
            fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (fd < 0) {
                throw std::system_error(errno, std::generic_category(), "Task completion eventfd");
            }
        }

        ~Channel()
        {
            close(fd);
        }

        void Signal()
        {
            const std::uint64_t one = 1;
            while (write(fd, &one, sizeof(one)) < 0 && errno == EINTR) {
            }
        }

        void Drain()
        {
            std::uint64_t count;
            while (read(fd, &count, sizeof(count)) < 0 && errno == EINTR) {
            }
        }

        int fd{-1};
        std::size_t outstanding_limit;
        std::size_t active_limit;
        bool watch_capacity;
        std::size_t active{};
        bool stopped{};
        std::unordered_map<std::uint64_t, std::shared_ptr<Job>> jobs;
        std::deque<TaskCompletion> completed;
    };

    enum class Phase { Queued, Active, Completed };

    struct Job {
        TaskRequest request;
        std::shared_ptr<Channel> channel;
        std::stop_source cancellation;
        Phase phase{Phase::Queued};
    };

    Impl(std::shared_ptr<SessionTaskBudget> shared_budget, std::size_t concurrency,
         std::size_t limit)
        : budget(std::move(shared_budget)), worker_limit(concurrency), outstanding_limit(limit)
    {
        if (concurrency == 0 || concurrency > 2 || limit == 0 || limit > 128) {
            throw std::invalid_argument("Task scheduler limits exceed supported bounds");
        }
        if (!budget) {
            budget = SessionTaskBudget::Create();
        }
        workers.reserve(worker_limit);
    }

    bool HasRunnable() const
    {
        if (stopped) {
            return true;
        }
        for (const auto &queue : queues) {
            for (const auto &job : queue) {
                if (job->channel->active < job->channel->active_limit) {
                    return true;
                }
            }
        }
        return false;
    }

    std::shared_ptr<Job> NextJob()
    {
        for (auto &queue : queues) {
            for (auto it = queue.begin(); it != queue.end(); ++it) {
                auto job = *it;
                if (job->channel->active >= job->channel->active_limit) {
                    continue;
                }
                queue.erase(it);
                job->phase = Phase::Active;
                ++job->channel->active;
                return job;
            }
        }
        return {};
    }

    void SignalCapacity()
    {
        for (const auto &weak : channels) {
            if (const auto channel = weak.lock();
                channel && !channel->stopped && channel->watch_capacity) {
                channel->Signal();
            }
        }
    }

    TaskCompletion Execute(const std::shared_ptr<Job> &job)
    {
        TaskCompletion result{job->request.id, {}, {}};
        const auto stop = job->cancellation.get_token();
        try {
            auto lease = budget->Acquire(job->request.reserve_bytes, job->request.priority, stop);
            if (!lease || stop.stop_requested()) {
                result.error = Cancellation();
                return result;
            }
            auto output = job->request.work(stop);
            if (stop.stop_requested()) {
                result.error = Cancellation();
                return result;
            }
            if (!output || output->retention_) {
                result.error = TaskError{TaskErrorCode::Work, "Task requires a fresh output"};
                return result;
            }
            if (!lease->Finish(output->RetainedBytes())) {
                result.error =
                    TaskError{TaskErrorCode::Budget, "Task output exceeds reserved budget"};
                return result;
            }
            output->retention_ = std::move(lease);
            result.output = std::move(output);
        } catch (const BudgetFailure &error) {
            result.error = TaskError{TaskErrorCode::Budget, error.what()};
        } catch (const std::exception &error) {
            result.error = TaskError{TaskErrorCode::Work, error.what()};
        } catch (...) {
            result.error = TaskError{TaskErrorCode::Work, "Unknown task exception"};
        }
        if (stop.stop_requested()) {
            result.output.reset();
            result.error = Cancellation();
        }
        return result;
    }

    void Run()
    {
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, std::bind_front(&Impl::HasRunnable, this));
                if (stopped) {
                    return;
                }
                job = NextJob();
            }

            auto completion = Execute(job);
            job->request.work = {};
            {
                std::lock_guard lock(mutex);
                auto &channel = *job->channel;
                --channel.active;
                if (stopped || channel.stopped) {
                    channel.jobs.erase(job->request.id);
                    --outstanding;
                    SignalCapacity();
                } else {
                    if (job->cancellation.stop_requested()) {
                        completion.output.reset();
                        completion.error = Cancellation();
                    }
                    job->phase = Phase::Completed;
                    channel.completed.push_back(std::move(completion));
                    channel.Signal();
                }
                wake.notify_all();
            }
        }
    }

    void DiscardChannel(const std::shared_ptr<Channel> &channel,
                        std::vector<std::stop_source> &cancellations)
    {
        const auto previous_outstanding = outstanding;
        channel->stopped = true;
        for (auto &queue : queues) {
            for (auto it = queue.begin(); it != queue.end();) {
                if ((*it)->channel == channel) {
                    cancellations.push_back((*it)->cancellation);
                    channel->jobs.erase((*it)->request.id);
                    --outstanding;
                    it = queue.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const auto &result : channel->completed) {
            channel->jobs.erase(result.id);
            --outstanding;
        }
        channel->completed.clear();
        channel->Drain();
        for (const auto &[id, job] : channel->jobs) {
            cancellations.push_back(job->cancellation);
        }
        if (outstanding != previous_outstanding) {
            SignalCapacity();
        }
    }

    bool ChannelStopped(const std::shared_ptr<Channel> &channel) const
    {
        return channel->active == 0;
    }

    void StopChannel(const std::shared_ptr<Channel> &channel)
    {
        std::vector<std::stop_source> cancellations;
        {
            std::lock_guard lock(mutex);
            DiscardChannel(channel, cancellations);
        }
        for (auto &stop : cancellations) {
            stop.request_stop();
        }
        wake.notify_all();
        std::unique_lock lock(mutex);
        wake.wait(lock, std::bind_front(&Impl::ChannelStopped, this, channel));
    }

    std::shared_ptr<SessionTaskBudget> budget;
    std::size_t worker_limit;
    std::size_t outstanding_limit;
    std::size_t outstanding{};
    bool stopped{};
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::array<std::deque<std::shared_ptr<Job>>, 3> queues;
    std::vector<std::thread> workers;
    std::vector<std::weak_ptr<Channel>> channels;
};

struct TaskChannel::Impl {
    std::shared_ptr<TaskScheduler::Impl> scheduler;
    std::shared_ptr<TaskScheduler::Impl::Channel> channel;
};

TaskScheduler::TaskScheduler(std::shared_ptr<SessionTaskBudget> budget, std::size_t workers,
                             std::size_t outstanding_limit)
    : impl_(std::make_shared<Impl>(std::move(budget), workers, outstanding_limit))
{
}

TaskScheduler::~TaskScheduler()
{
    Stop();
}

std::shared_ptr<TaskChannel> TaskScheduler::OpenChannel(std::size_t outstanding_limit,
                                                        std::size_t active_limit,
                                                        bool capacity_progress)
{
    if (outstanding_limit == 0 || outstanding_limit > 128 || active_limit == 0 ||
        active_limit > 2) {
        throw std::invalid_argument("Task channel limits exceed supported bounds");
    }
    auto channel =
        std::make_shared<Impl::Channel>(outstanding_limit, active_limit, capacity_progress);
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopped) {
        throw std::logic_error("Task scheduler is stopped");
    }
    std::erase_if(impl_->channels, [](const auto &weak) { return weak.expired(); });
    impl_->channels.push_back(channel);
    return std::shared_ptr<TaskChannel>(new TaskChannel(
        std::make_shared<TaskChannel::Impl>(TaskChannel::Impl{impl_, std::move(channel)})));
}

void TaskScheduler::Stop()
{
    std::vector<std::stop_source> cancellations;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopped = true;
        for (const auto &weak : impl_->channels) {
            if (auto channel = weak.lock()) {
                impl_->DiscardChannel(channel, cancellations);
            }
        }
    }
    for (auto &stop : cancellations) {
        stop.request_stop();
    }
    impl_->wake.notify_all();
    for (auto &worker : impl_->workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

TaskSchedulerStats TaskScheduler::Stats() const
{
    std::lock_guard lock(impl_->mutex);
    TaskSchedulerStats result;
    result.workers = impl_->workers.size();
    result.outstanding = impl_->outstanding;
    for (const auto &queue : impl_->queues) {
        result.queued += queue.size();
    }
    for (const auto &weak : impl_->channels) {
        if (const auto channel = weak.lock()) {
            result.active += channel->active;
            result.completed += channel->completed.size();
        }
    }
    return result;
}

TaskChannel::TaskChannel(std::shared_ptr<Impl> impl) : impl_(std::move(impl))
{
}

TaskChannel::~TaskChannel()
{
    Stop();
}

TaskSubmitResult TaskChannel::Submit(TaskRequest request)
{
    auto &scheduler = *impl_->scheduler;
    const auto &channel = impl_->channel;
    std::lock_guard lock(scheduler.mutex);
    if (scheduler.stopped || channel->stopped) {
        return TaskSubmitResult::Closed;
    }
    const auto priority = static_cast<std::size_t>(request.priority);
    if (!request.id || !request.work || !request.reserve_bytes || priority >= 3 ||
        channel->jobs.contains(request.id)) {
        return TaskSubmitResult::Invalid;
    }
    if (scheduler.outstanding >= scheduler.outstanding_limit ||
        channel->jobs.size() >= channel->outstanding_limit) {
        return TaskSubmitResult::Busy;
    }

    auto job = std::make_shared<TaskScheduler::Impl::Job>();
    job->request = std::move(request);
    job->channel = channel;
    channel->jobs.emplace(job->request.id, job);
    try {
        scheduler.queues[priority].push_back(job);
    } catch (...) {
        channel->jobs.erase(job->request.id);
        throw;
    }
    ++scheduler.outstanding;
    if (scheduler.workers.size() < scheduler.worker_limit &&
        scheduler.workers.size() < scheduler.outstanding) {
        try {
            scheduler.workers.emplace_back(&TaskScheduler::Impl::Run, &scheduler);
        } catch (...) {
            scheduler.queues[priority].pop_back();
            channel->jobs.erase(job->request.id);
            --scheduler.outstanding;
            scheduler.SignalCapacity();
            throw;
        }
    }
    scheduler.wake.notify_all();
    return TaskSubmitResult::Accepted;
}

void TaskChannel::Cancel(std::uint64_t id)
{
    auto &scheduler = *impl_->scheduler;
    const auto &channel = impl_->channel;
    std::optional<std::stop_source> cancellation;
    {
        std::lock_guard lock(scheduler.mutex);
        const auto found = channel->jobs.find(id);
        if (found == channel->jobs.end()) {
            return;
        }
        cancellation = found->second->cancellation;
        for (auto &completion : channel->completed) {
            if (completion.id == id) {
                completion.output.reset();
                completion.error = Cancellation();
            }
        }
    }
    cancellation->request_stop();
    {
        std::lock_guard lock(scheduler.mutex);
        for (auto &completion : channel->completed) {
            if (completion.id == id) {
                completion.output.reset();
                completion.error = Cancellation();
            }
        }
    }
    scheduler.wake.notify_all();
}

void TaskChannel::Stop()
{
    impl_->scheduler->StopChannel(impl_->channel);
}

int TaskChannel::Fd() const noexcept
{
    return impl_->channel->fd;
}

std::optional<TaskCompletion> TaskChannel::TakeCompletion()
{
    auto &scheduler = *impl_->scheduler;
    const auto &channel = impl_->channel;
    std::lock_guard lock(scheduler.mutex);
    channel->Drain();
    if (channel->completed.empty()) {
        return {};
    }
    auto result = std::move(channel->completed.front());
    channel->completed.pop_front();
    channel->jobs.erase(result.id);
    --scheduler.outstanding;
    scheduler.SignalCapacity();
    if (!channel->watch_capacity && !channel->completed.empty()) {
        channel->Signal();
    }
    scheduler.wake.notify_all();
    return result;
}

} // namespace prism::runtime
