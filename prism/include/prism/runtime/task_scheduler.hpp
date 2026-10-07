#pragma once

#include "prism/runtime/session_task_budget.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace prism::runtime {

class TaskScheduler;

class TaskOutput {
public:
    virtual ~TaskOutput() = default;
    virtual std::uint64_t RetainedBytes() const noexcept = 0;

protected:
    TaskOutput() = default;
    TaskOutput(const TaskOutput &) = delete;
    TaskOutput &operator=(const TaskOutput &) = delete;

private:
    friend class TaskScheduler;
    mutable std::shared_ptr<TaskLease> retention_;
};

// Work owns its input and returns a fresh immutable output. It may only perform
// Background reading/preparation only, with cancellation checked at operation
// boundaries. It must not mutate business/UI state or retain frontend/platform
// objects or source views. Blocking filesystem calls cannot be forcibly interrupted.
using TaskWork = std::function<std::shared_ptr<const TaskOutput>(std::stop_token)>;

struct TaskRequest {
    std::uint64_t id{};
    TaskPriority priority{TaskPriority::Critical};
    std::uint64_t reserve_bytes{};
    TaskWork work;
};

enum class TaskErrorCode { Cancelled, Budget, Work };

struct TaskError {
    TaskErrorCode code{TaskErrorCode::Work};
    std::string message;
};

struct TaskCompletion {
    std::uint64_t id{};
    std::shared_ptr<const TaskOutput> output;
    std::optional<TaskError> error;
};

enum class TaskSubmitResult { Accepted, Busy, Closed, Invalid };

struct TaskSchedulerStats {
    std::size_t workers{}, outstanding{}, queued{}, active{}, completed{};
};

class TaskChannel {
public:
    ~TaskChannel();
    TaskChannel(const TaskChannel &) = delete;
    TaskChannel &operator=(const TaskChannel &) = delete;
    TaskSubmitResult Submit(TaskRequest request);
    void Cancel(std::uint64_t id);
    // Cancels and discards this channel, waits for its active work only.
    void Stop();
    // Readable for completed work and, if enabled, shared queue capacity
    // progress. A progress wake may return no completion.
    int Fd() const noexcept;
    std::optional<TaskCompletion> TakeCompletion();

private:
    friend class TaskScheduler;
    struct Impl;
    explicit TaskChannel(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

class TaskScheduler {
public:
    explicit TaskScheduler(std::shared_ptr<SessionTaskBudget> budget = {}, std::size_t workers = 2,
                           std::size_t outstanding_limit = 128);
    ~TaskScheduler();
    TaskScheduler(const TaskScheduler &) = delete;
    TaskScheduler &operator=(const TaskScheduler &) = delete;
    std::shared_ptr<TaskChannel> OpenChannel(std::size_t outstanding_limit = 128,
                                             std::size_t active_limit = 2,
                                             bool capacity_progress = true);
    void Stop();
    TaskSchedulerStats Stats() const;

private:
    friend class TaskChannel;
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace prism::runtime
