#include "session_task_budget_p.hpp"
#include <limits>
#include <unistd.h>

namespace prism::runtime {
namespace {
using detail::LeaseState;
using detail::SharedBudget;
using detail::SharedLease;

constexpr std::uint32_t NoSlot = SessionTaskBudget::MaxLeases;

detail::SharedOwner &Owner(SharedBudget &shared, pid_t pid)
{
    detail::SharedOwner *empty = nullptr;
    for (auto &owner : shared.owners) {
        if (owner.pid == pid) {
            return owner;
        }
        if (!owner.pid && !empty) {
            empty = &owner;
        }
    }
    if (!empty) {
        for (auto &candidate : shared.owners) {
            bool in_use = false;
            for (const auto &lease : shared.leases) {
                if (lease.owner == candidate.pid && lease.state != LeaseState::Empty) {
                    in_use = true;
                    break;
                }
            }
            if (!in_use && (!empty || candidate.turn < empty->turn)) {
                empty = &candidate;
            }
        }
    }
    if (!empty) {
        throw BudgetFailure(BudgetError::Capacity, "Session task budget owner table is full");
    }
    empty->turn = 0;
    empty->pid = pid;
    return *empty;
}

void Remove(SharedBudget &shared, std::uint32_t slot) noexcept
{
    detail::Publish(shared.leases[slot], LeaseState::Empty);
    // Keep the owner's fairness turn through gaps between its tasks. Empty
    // owner entries are reclaimed only under table pressure or explicit reap.
    detail::Rebuild(shared);
    detail::Notify(shared);
}

std::uint32_t Select(SharedBudget &shared)
{
    std::uint32_t selected = NoSlot;
    std::uint64_t selected_turn{};
    const auto available = shared.config.memory_limit - shared.stats.reserved_bytes;
    for (const auto &owner : shared.owners) {
        if (!owner.pid) {
            continue;
        }
        // FIFO is maintained within each owner and priority class. A blocked
        // large reservation does not strand a different owner that can run.
        std::array<std::uint32_t, 3> first{NoSlot, NoSlot, NoSlot};
        for (std::uint32_t index = 0; index < shared.leases.size(); ++index) {
            const auto &lease = shared.leases[index];
            if (lease.state != LeaseState::Waiting || lease.owner != owner.pid) {
                continue;
            }
            const auto priority = static_cast<std::uint32_t>(lease.priority);
            if (first[priority] == NoSlot || lease.serial < shared.leases[first[priority]].serial) {
                first[priority] = index;
            }
        }
        for (const auto index : first) {
            if (index == NoSlot || shared.leases[index].bytes > available) {
                continue;
            }
            const auto &candidate = shared.leases[index];
            if (selected == NoSlot || candidate.priority < shared.leases[selected].priority ||
                (candidate.priority == shared.leases[selected].priority &&
                 (owner.turn < selected_turn ||
                  (owner.turn == selected_turn &&
                   candidate.serial < shared.leases[selected].serial)))) {
                selected = index;
                selected_turn = owner.turn;
            }
            break;
        }
    }
    return selected;
}

std::uint32_t Register(SharedBudget &shared, std::uint64_t bytes, TaskPriority priority, pid_t pid)
{
    if (shared.stats.waiting >= SessionTaskBudget::MaxWaiting ||
        shared.next_serial == std::numeric_limits<std::uint64_t>::max() ||
        shared.next_grant == std::numeric_limits<std::uint64_t>::max()) {
        throw BudgetFailure(BudgetError::Capacity,
                            "Session task budget waiting capacity exhausted");
    }
    std::uint32_t slot = NoSlot;
    for (std::uint32_t index = 0; index < shared.leases.size(); ++index) {
        if (shared.leases[index].state == LeaseState::Empty) {
            slot = index;
            break;
        }
    }
    if (slot == NoSlot) {
        throw BudgetFailure(BudgetError::Capacity, "Session task budget lease capacity exhausted");
    }
    Owner(shared, pid);
    auto &lease = shared.leases[slot];
    lease.owner = pid;
    lease.priority = priority;
    lease.serial = shared.next_serial++;
    lease.bytes = bytes;
    lease.grant = 0;
    detail::Publish(lease, LeaseState::Waiting);
    detail::Rebuild(shared);
    detail::Notify(shared);
    return slot;
}
} // namespace

std::shared_ptr<TaskLease> SessionTaskBudget::Acquire(std::uint64_t bytes, TaskPriority priority,
                                                      std::stop_token stop)
{
    if (static_cast<unsigned>(priority) > static_cast<unsigned>(TaskPriority::Deferred)) {
        throw BudgetFailure(BudgetError::InvalidConfig, "Invalid task priority");
    }
    if (bytes > impl_->shared->config.memory_limit) {
        throw BudgetFailure(BudgetError::ReservationTooLarge, "Task exceeds session memory budget");
    }
    if (stop.stop_requested()) {
        return {};
    }

    const auto owner = getpid();
    // Allocate the shared ownership control block before publishing a lease.
    // Its destructor is inert until a successful grant sets serial_.
    auto result = std::shared_ptr<TaskLease>(new TaskLease(shared_from_this(), NoSlot, 0, owner));
    std::stop_callback<CancelNotifier> cancel(stop, CancelNotifier{shared_from_this()});
    {
        auto &shared = *impl_->shared;
        detail::BudgetLock lock(shared);
        if (stop.stop_requested()) {
            return {};
        }
        const auto slot = Register(shared, bytes, priority, owner);
        const auto serial = shared.leases[slot].serial;
        try {
            while (true) {
                if (!detail::Matches(shared.leases[slot], serial, owner)) {
                    return {};
                }
                if (stop.stop_requested()) {
                    Remove(shared, slot);
                    return {};
                }
                if (!shared.stats.active &&
                    bytes > shared.config.memory_limit - shared.stats.retained_bytes) {
                    throw BudgetFailure(BudgetError::RetainedPressure,
                                        "Retained results prevent this task reservation");
                }
                if (shared.stats.active < shared.config.active_limit && Select(shared) == slot) {
                    if (shared.next_grant == std::numeric_limits<std::uint64_t>::max()) {
                        throw BudgetFailure(BudgetError::Capacity,
                                            "Session task grant identity exhausted");
                    }
                    auto &lease = shared.leases[slot];
                    lease.grant = shared.next_grant++;
                    detail::Publish(lease, LeaseState::Active);
                    Owner(shared, owner).turn = lease.grant;
                    detail::Rebuild(shared);
                    detail::Notify(shared);
                    result->slot_ = slot;
                    result->serial_ = serial;
                    break;
                }
                lock.Wait();
            }
        } catch (...) {
            if (lock.Owns() && detail::Matches(shared.leases[slot], serial, owner)) {
                Remove(shared, slot);
            }
            throw;
        }
    }
    // Neither stop_callback destruction nor lease destruction may run locked:
    // an already-running cancellation notifier also needs the shared mutex.
    return result;
}

bool SessionTaskBudget::Finish(std::uint32_t slot, std::uint64_t serial, pid_t owner,
                               std::uint64_t retained_bytes) noexcept
{
    if (owner != getpid() || slot >= MaxLeases) {
        return false;
    }
    try {
        auto &shared = *impl_->shared;
        detail::BudgetLock lock(shared);
        auto &lease = shared.leases[slot];
        if (!detail::Matches(lease, serial, owner)) {
            return false;
        }
        if (lease.state == LeaseState::Retained) {
            return lease.bytes == retained_bytes;
        }
        if (lease.state != LeaseState::Active || retained_bytes > lease.bytes ||
            retained_bytes > shared.config.memory_limit - shared.config.working_headroom -
                                 shared.stats.retained_bytes) {
            return false;
        }
        lease.bytes = retained_bytes;
        detail::Publish(lease, LeaseState::Retained);
        detail::Rebuild(shared);
        detail::Notify(shared);
        return true;
    } catch (...) {
        return false;
    }
}

void SessionTaskBudget::Release(std::uint32_t slot, std::uint64_t serial, pid_t owner) noexcept
{
    if (owner != getpid() || slot >= MaxLeases) {
        return;
    }
    try {
        auto &shared = *impl_->shared;
        detail::BudgetLock lock(shared);
        if (detail::Matches(shared.leases[slot], serial, owner)) {
            Remove(shared, slot);
        }
    } catch (...) {
    }
}

std::uint64_t SessionTaskBudget::Bytes(std::uint32_t slot, std::uint64_t serial,
                                       pid_t owner) const noexcept
{
    if (slot >= MaxLeases || owner != getpid()) {
        return 0;
    }
    try {
        auto &shared = *impl_->shared;
        detail::BudgetLock lock(shared);
        const auto &lease = shared.leases[slot];
        return detail::Matches(lease, serial, owner) ? lease.bytes : 0;
    } catch (...) {
        return 0;
    }
}

TaskLease::TaskLease(std::shared_ptr<SessionTaskBudget> budget, std::uint32_t slot,
                     std::uint64_t serial, pid_t owner)
    : budget_(std::move(budget)), slot_(slot), serial_(serial), owner_(owner)
{
}

TaskLease::~TaskLease()
{
    if (serial_) {
        budget_->Release(slot_, serial_, owner_);
    }
}

bool TaskLease::Finish(std::uint64_t retained_bytes) noexcept
{
    return budget_->Finish(slot_, serial_, owner_, retained_bytes);
}

std::uint64_t TaskLease::Bytes() const noexcept
{
    return budget_->Bytes(slot_, serial_, owner_);
}
} // namespace prism::runtime
