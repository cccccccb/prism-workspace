#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <sys/types.h>

namespace prism::runtime {
enum class TaskPriority : std::uint8_t { Critical = 0, Resource = 1, Deferred = 2 };

struct TaskBudgetConfig {
    std::uint32_t active_limit{2};
    std::uint64_t memory_limit{256ULL * 1024 * 1024};
    std::uint64_t working_headroom{64ULL * 1024 * 1024};
};

struct TaskBudgetStats {
    std::uint32_t active{}, waiting{}, retained_leases{}, leases{};
    std::uint64_t reserved_bytes{}, retained_bytes{}, recoveries{};
};

enum class BudgetError {
    InvalidConfig,
    InvalidHandle,
    Capacity,
    ReservationTooLarge,
    RetainedPressure,
    Synchronization
};

class BudgetFailure : public std::runtime_error {
public:
    BudgetFailure(BudgetError code, const char *message);
    BudgetError Code() const noexcept;

private:
    BudgetError code_;
};

class SessionTaskBudget;

// Shared ownership keeps both the reservation and its mapped session alive.
// Finish never increases a reservation, and fails if retained headroom is exhausted.
class TaskLease {
public:
    ~TaskLease();
    TaskLease(const TaskLease &) = delete;
    TaskLease &operator=(const TaskLease &) = delete;
    bool Finish(std::uint64_t retained_bytes) noexcept;
    std::uint64_t Bytes() const noexcept;

private:
    friend class SessionTaskBudget;
    TaskLease(std::shared_ptr<SessionTaskBudget> budget, std::uint32_t slot, std::uint64_t serial,
              pid_t owner);
    std::shared_ptr<SessionTaskBudget> budget_;
    std::uint32_t slot_;
    std::uint64_t serial_;
    pid_t owner_;
};

class SessionTaskBudget : public std::enable_shared_from_this<SessionTaskBudget> {
public:
    static constexpr std::uint32_t MaxWaiting = 128;
    static constexpr std::uint32_t MaxLeases = 2048;
    static std::shared_ptr<SessionTaskBudget> Create(TaskBudgetConfig config = {});
    static std::shared_ptr<SessionTaskBudget> Attach(int fd, pid_t expected_creator_pid = 0);
    ~SessionTaskBudget();
    SessionTaskBudget(const SessionTaskBudget &) = delete;
    SessionTaskBudget &operator=(const SessionTaskBudget &) = delete;

    int Fd() const noexcept;
    // Only scheduler workers may wait here. Cancellation returns an empty pointer.
    std::shared_ptr<TaskLease> Acquire(std::uint64_t bytes, TaskPriority priority,
                                       std::stop_token stop = {});
    void DropProcess(pid_t process);
    TaskBudgetStats Stats() const;

private:
    friend class TaskLease;
    struct Impl;
    explicit SessionTaskBudget(std::unique_ptr<Impl> impl);
    bool Finish(std::uint32_t slot, std::uint64_t serial, pid_t owner,
                std::uint64_t retained_bytes) noexcept;
    void Release(std::uint32_t slot, std::uint64_t serial, pid_t owner) noexcept;
    std::uint64_t Bytes(std::uint32_t slot, std::uint64_t serial, pid_t owner) const noexcept;
    void Wake() noexcept;

    struct CancelNotifier {
        std::shared_ptr<SessionTaskBudget> budget;
        void operator()() const noexcept;
    };

    std::unique_ptr<Impl> impl_;
};
} // namespace prism::runtime
