#include "session_task_budget_p.hpp"
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <linux/futex.h>
#include <new>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace prism::runtime {
namespace {
void CheckSync(int result)
{
    if (result) {
        throw BudgetFailure(BudgetError::Synchronization, "Task budget synchronization failed");
    }
}

void Initialize(detail::SharedBudget &shared, TaskBudgetConfig config)
{
    pthread_mutexattr_t mutex_attribute;
    CheckSync(pthread_mutexattr_init(&mutex_attribute));
    const int shared_mutex = pthread_mutexattr_setpshared(&mutex_attribute, PTHREAD_PROCESS_SHARED);
    const int robust_mutex = pthread_mutexattr_setrobust(&mutex_attribute, PTHREAD_MUTEX_ROBUST);
    const int initialized_mutex =
        shared_mutex || robust_mutex ? EINVAL : pthread_mutex_init(&shared.mutex, &mutex_attribute);
    pthread_mutexattr_destroy(&mutex_attribute);
    CheckSync(initialized_mutex);

    shared.config = config;
    shared.next_serial = 1;
    shared.next_grant = 1;
    shared.creator = getpid();
    shared.uid = geteuid();
    shared.version = detail::BudgetVersion;
    shared.mapping_bytes = sizeof(shared);
    shared.magic = detail::BudgetMagic;
}

void WakeWaiters(detail::SharedBudget &shared) noexcept
{
    syscall(SYS_futex, &shared.changed, FUTEX_WAKE, std::numeric_limits<int>::max(), nullptr,
            nullptr, 0);
}

detail::SharedBudget *Map(int fd)
{
    void *address =
        mmap(nullptr, sizeof(detail::SharedBudget), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (address == MAP_FAILED) {
        throw BudgetFailure(BudgetError::InvalidHandle, "Cannot map session task budget");
    }
    return static_cast<detail::SharedBudget *>(address);
}
} // namespace

namespace detail {
void ValidateConfig(TaskBudgetConfig config)
{
    if (!config.active_limit || config.active_limit > SessionTaskBudget::MaxWaiting ||
        !config.memory_limit || config.working_headroom > config.memory_limit ||
        config.memory_limit >
            std::numeric_limits<std::uint64_t>::max() / SessionTaskBudget::MaxLeases) {
        throw BudgetFailure(BudgetError::InvalidConfig, "Invalid session task budget limits");
    }
}

void Rebuild(SharedBudget &shared) noexcept
{
    const auto recoveries = shared.stats.recoveries;
    shared.stats = {};
    shared.stats.recoveries = recoveries;
    for (const auto &lease : shared.leases) {
        if (lease.state == LeaseState::Empty) {
            continue;
        }
        ++shared.stats.leases;
        if (lease.state == LeaseState::Waiting) {
            ++shared.stats.waiting;
        } else {
            shared.stats.reserved_bytes += lease.bytes;
            if (lease.state == LeaseState::Active) {
                ++shared.stats.active;
            } else {
                ++shared.stats.retained_leases;
                shared.stats.retained_bytes += lease.bytes;
            }
        }
    }
}

void Recover(SharedBudget &shared)
{
    Rebuild(shared);
    ++shared.stats.recoveries;
    // A grant publishes its table state before its fairness turn. Repair that
    // possible interrupted write from the authoritative granted lease.
    for (auto &owner : shared.owners) {
        for (const auto &lease : shared.leases) {
            if (lease.owner == owner.pid && lease.state != LeaseState::Empty &&
                lease.grant > owner.turn) {
                owner.turn = lease.grant;
            }
        }
    }
    CheckSync(pthread_mutex_consistent(&shared.mutex));
    Notify(shared);
}

void Notify(SharedBudget &shared) noexcept
{
    shared.changed.fetch_add(1, std::memory_order_release);
}

void Publish(SharedLease &lease, LeaseState state) noexcept
{
    // Recovery must never observe a live state before its PID/serial/size.
    std::atomic_thread_fence(std::memory_order_release);
    lease.state = state;
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

bool Matches(const SharedLease &lease, std::uint64_t serial, pid_t owner) noexcept
{
    return lease.state != LeaseState::Empty && lease.serial == serial && lease.owner == owner;
}

BudgetLock::BudgetLock(SharedBudget &shared)
    : shared_(shared), observed_(shared.changed.load(std::memory_order_acquire))
{
    Lock();
}

void BudgetLock::Lock()
{
    const int result = pthread_mutex_lock(&shared_.mutex);
    if (result == EOWNERDEAD) {
        locked_ = true;
        try {
            Recover(shared_);
        } catch (...) {
            Unlock();
            throw;
        }
    } else {
        CheckSync(result);
        locked_ = true;
    }
}

BudgetLock::~BudgetLock()
{
    Unlock();
}

void BudgetLock::Unlock() noexcept
{
    if (!locked_) {
        return;
    }
    const auto published = shared_.changed.load(std::memory_order_acquire);
    pthread_mutex_unlock(&shared_.mutex);
    locked_ = false;
    // The sequence is published while locked; the kernel wake occurs after
    // unlocking. A killed notifier cannot leave a second user-space lock held.
    if (published != observed_) {
        WakeWaiters(shared_);
    }
    observed_ = published;
}

void BudgetLock::Wait()
{
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(sizeof(shared_.changed) == sizeof(std::uint32_t));
    const auto expected = shared_.changed.load(std::memory_order_acquire);
    Unlock();
    const auto result =
        syscall(SYS_futex, &shared_.changed, FUTEX_WAIT, expected, nullptr, nullptr, 0);
    const auto error = errno;
    Lock();
    if (result < 0 && error != EAGAIN && error != EINTR) {
        CheckSync(error);
    }
}

bool BudgetLock::Owns() const noexcept
{
    return locked_;
}
} // namespace detail

BudgetFailure::BudgetFailure(BudgetError code, const char *message)
    : std::runtime_error(message), code_(code)
{
}

BudgetError BudgetFailure::Code() const noexcept
{
    return code_;
}

SessionTaskBudget::Impl::~Impl()
{
    // Other processes may still use the mapping; do not destroy its shared
    // mutex here. The last memfd reference lets the kernel reclaim storage.
    if (shared) {
        munmap(shared, sizeof(*shared));
    }
    if (fd >= 0) {
        close(fd);
    }
}

SessionTaskBudget::SessionTaskBudget(std::unique_ptr<Impl> impl) : impl_(std::move(impl))
{
}

SessionTaskBudget::~SessionTaskBudget() = default;

std::shared_ptr<SessionTaskBudget> SessionTaskBudget::Create(TaskBudgetConfig config)
{
    detail::ValidateConfig(config);
    auto impl = std::make_unique<Impl>();
    impl->fd = memfd_create("prism-session-load-budget", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (impl->fd < 0 || ftruncate(impl->fd, sizeof(detail::SharedBudget))) {
        throw BudgetFailure(BudgetError::InvalidHandle, "Cannot create session task budget");
    }
    impl->shared = Map(impl->fd);
    new (impl->shared) detail::SharedBudget{};
    Initialize(*impl->shared, config);
    if (fcntl(impl->fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0) {
        throw BudgetFailure(BudgetError::InvalidHandle, "Cannot seal session task budget size");
    }
    return std::shared_ptr<SessionTaskBudget>(new SessionTaskBudget(std::move(impl)));
}

std::shared_ptr<SessionTaskBudget> SessionTaskBudget::Attach(int fd, pid_t expected_creator_pid)
{
    struct stat info{};
    const int seals = fcntl(fd, F_GET_SEALS);
    const int required_seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    if (fd < 0 || fstat(fd, &info) || !S_ISREG(info.st_mode) ||
        info.st_size != static_cast<off_t>(sizeof(detail::SharedBudget)) || seals < 0 ||
        (seals & required_seals) != required_seals || (seals & F_SEAL_WRITE)) {
        throw BudgetFailure(BudgetError::InvalidHandle, "Invalid session task budget descriptor");
    }

    auto impl = std::make_unique<Impl>();
    impl->fd = fcntl(fd, F_DUPFD_CLOEXEC, 10);
    if (impl->fd < 0) {
        throw BudgetFailure(BudgetError::InvalidHandle,
                            "Cannot retain session task budget descriptor");
    }
    impl->shared = Map(impl->fd);
    const auto &shared = *impl->shared;
    if (shared.magic != detail::BudgetMagic || shared.version != detail::BudgetVersion ||
        shared.mapping_bytes != sizeof(shared) || shared.uid != geteuid() || shared.creator <= 1 ||
        (expected_creator_pid && expected_creator_pid != shared.creator)) {
        throw BudgetFailure(BudgetError::InvalidHandle,
                            "Session task budget identity/version mismatch");
    }
    detail::ValidateConfig(shared.config);
    return std::shared_ptr<SessionTaskBudget>(new SessionTaskBudget(std::move(impl)));
}

int SessionTaskBudget::Fd() const noexcept
{
    return impl_->fd;
}

TaskBudgetStats SessionTaskBudget::Stats() const
{
    detail::BudgetLock lock(*impl_->shared);
    return impl_->shared->stats;
}

void SessionTaskBudget::DropProcess(pid_t process)
{
    if (process <= 0) {
        return;
    }
    auto &shared = *impl_->shared;
    detail::BudgetLock lock(shared);
    for (auto &lease : shared.leases) {
        if (lease.owner == process) {
            detail::Publish(lease, detail::LeaseState::Empty);
        }
    }
    for (auto &owner : shared.owners) {
        if (owner.pid == process) {
            owner = {};
        }
    }
    detail::Rebuild(shared);
    detail::Notify(shared);
}

void SessionTaskBudget::Wake() noexcept
{
    try {
        detail::BudgetLock lock(*impl_->shared);
        detail::Notify(*impl_->shared);
    } catch (...) {
        // A corrupt/unrecoverable region is reported by the worker's Acquire.
    }
}

void SessionTaskBudget::CancelNotifier::operator()() const noexcept
{
    budget->Wake();
}
} // namespace prism::runtime
