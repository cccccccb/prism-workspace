#pragma once

#include "prism/runtime/session_task_budget.hpp"
#include <array>
#include <atomic>
#include <pthread.h>

namespace prism::runtime::detail {
inline constexpr std::uint64_t BudgetMagic = 0x505249534d425544ULL;
inline constexpr std::uint32_t BudgetVersion = 1;
enum class LeaseState : std::uint32_t { Empty, Waiting, Active, Retained };

struct SharedLease {
    LeaseState state{};
    pid_t owner{};
    TaskPriority priority{};
    std::uint64_t serial{}, bytes{}, grant{};
};

struct SharedOwner {
    pid_t pid{};
    std::uint64_t turn{};
};

// No C++ ownership or process-local pointers may be placed in this mapping.
struct SharedBudget {
    std::uint64_t magic{};
    std::uint32_t version{}, mapping_bytes{};
    pid_t creator{};
    uid_t uid{};
    TaskBudgetConfig config;
    pthread_mutex_t mutex;
    alignas(4) std::atomic<std::uint32_t> changed{};
    std::uint64_t next_serial{1}, next_grant{1};
    TaskBudgetStats stats;
    std::array<SharedOwner, SessionTaskBudget::MaxWaiting> owners;
    std::array<SharedLease, SessionTaskBudget::MaxLeases> leases;
};

void ValidateConfig(TaskBudgetConfig config);
void Rebuild(SharedBudget &shared) noexcept;
void Recover(SharedBudget &shared);
void Notify(SharedBudget &shared) noexcept;
void Publish(SharedLease &lease, LeaseState state) noexcept;
bool Matches(const SharedLease &lease, std::uint64_t serial, pid_t owner) noexcept;

class BudgetLock {
public:
    explicit BudgetLock(SharedBudget &shared);
    ~BudgetLock();
    BudgetLock(const BudgetLock &) = delete;
    BudgetLock &operator=(const BudgetLock &) = delete;
    void Wait();
    bool Owns() const noexcept;

private:
    SharedBudget &shared_;
    std::uint32_t observed_{};
    bool locked_{};
    void Lock();
    void Unlock() noexcept;
};
} // namespace prism::runtime::detail

namespace prism::runtime {
struct SessionTaskBudget::Impl {
    int fd{-1};
    detail::SharedBudget *shared{};
    ~Impl();
};
} // namespace prism::runtime
