#include "../prism/runtime/session_task_budget_p.hpp"
#include "prism/runtime/session_task_budget.hpp"
#include <cassert>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <memory>
#include <pthread.h>
#include <stop_token>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace prism::runtime;

struct Pipe {
    int read{}, write{};

    Pipe()
    {
        int pair[2];
        assert(pipe2(pair, O_CLOEXEC) == 0);
        read = pair[0];
        write = pair[1];
    }

    ~Pipe()
    {
        close(read);
        close(write);
    }
};

void Send(int fd, char value)
{
    ssize_t sent;
    do {
        sent = write(fd, &value, 1);
    } while (sent < 0 && errno == EINTR);
    assert(sent == 1);
}

char Receive(int fd)
{
    char value{};
    ssize_t received;
    do {
        received = read(fd, &value, 1);
    } while (received < 0 && errno == EINTR);
    assert(received == 1);
    return value;
}

void WaitChild(pid_t pid, bool killed = false)
{
    int status{};
    pid_t result;
    do {
        result = waitpid(pid, &status, 0);
    } while (result < 0 && errno == EINTR);
    assert(result == pid);
    if (killed) {
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    } else {
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}

class Mapping {
public:
    explicit Mapping(int fd)
    {
        void *mapped =
            mmap(nullptr, sizeof(detail::SharedBudget), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        assert(mapped != MAP_FAILED);
        shared_ = static_cast<detail::SharedBudget *>(mapped);
    }

    ~Mapping()
    {
        munmap(shared_, sizeof(*shared_));
    }

    detail::SharedBudget &Get()
    {
        return *shared_;
    }

private:
    detail::SharedBudget *shared_{};
};

// Tests observe the same sequence transition used by waiting workers. There
// are no fixed sleeps, polling loops or production test hooks.
void WaitWaiting(const std::shared_ptr<SessionTaskBudget> &budget, unsigned count)
{
    Mapping mapping(budget->Fd());
    auto &shared = mapping.Get();
    detail::BudgetLock lock(shared);
    while (shared.stats.waiting != count) {
        lock.Wait();
    }
}

void VerifyEmpty(const std::shared_ptr<SessionTaskBudget> &budget)
{
    const auto stats = budget->Stats();
    assert(stats.active == 0 && stats.waiting == 0 && stats.leases == 0);
    assert(stats.reserved_bytes == 0 && stats.retained_bytes == 0);
}

void ExpectAcquireFailure(const std::shared_ptr<SessionTaskBudget> &budget, std::uint64_t bytes,
                          BudgetError expected)
{
    bool failed = false;
    try {
        budget->Acquire(bytes, TaskPriority::Critical);
    } catch (const BudgetFailure &error) {
        assert(error.Code() == expected);
        failed = true;
    }
    assert(failed);
}

struct ReleaseLease {
    std::shared_ptr<TaskLease> *lease;

    void operator()() const
    {
        lease->reset();
    }
};

void RetentionAndHeadroom()
{
    auto budget = SessionTaskBudget::Create({1, 100, 40});
    auto attached = SessionTaskBudget::Attach(budget->Fd(), getpid());
    auto result = attached->Acquire(70, TaskPriority::Critical);
    assert(result && result->Bytes() == 70);
    assert(!result->Finish(71));
    assert(budget->Stats().active == 1);
    assert(result->Finish(60) && result->Finish(60));
    assert(!result->Finish(59));
    auto stats = budget->Stats();
    assert(stats.active == 0 && stats.retained_bytes == 60 && stats.reserved_bytes == 60);
    ExpectAcquireFailure(budget, 101, BudgetError::ReservationTooLarge);
    ExpectAcquireFailure(budget, 41, BudgetError::RetainedPressure);

    auto working = budget->Acquire(40, TaskPriority::Resource);
    assert(working && !working->Finish(1));
    assert(working->Bytes() == 40 && budget->Stats().active == 1);
    assert(working->Finish(0));
    working.reset();
    std::thread release(ReleaseLease{&result});
    release.join();
    VerifyEmpty(budget);
}

struct AcquireThread {
    std::shared_ptr<SessionTaskBudget> budget;
    std::stop_token stop;
    std::shared_ptr<TaskLease> *result{};
    int ready{-1};
    TaskPriority priority{TaskPriority::Critical};

    void operator()() const
    {
        *result = budget->Acquire(20, priority, stop);
        if (ready >= 0) {
            Send(ready, *result ? 'y' : 'n');
        }
    }
};

void CancellationAndFinishWake()
{
    auto budget = SessionTaskBudget::Create({1, 100, 0});
    auto first = budget->Acquire(40, TaskPriority::Critical);
    std::stop_source cancel;
    std::shared_ptr<TaskLease> waiting;
    std::thread worker(AcquireThread{budget, cancel.get_token(), &waiting});
    WaitWaiting(budget, 1);
    assert(cancel.request_stop());
    worker.join();
    assert(!waiting && budget->Stats().waiting == 0 && budget->Stats().active == 1);

    std::thread retry(AcquireThread{budget, {}, &waiting});
    WaitWaiting(budget, 1);
    assert(first->Finish(30));
    retry.join();
    assert(waiting && budget->Stats().active == 1 && budget->Stats().retained_bytes == 30);
    first.reset();
    waiting.reset();
    assert(!budget->Acquire(10, TaskPriority::Critical, cancel.get_token()));
    VerifyEmpty(budget);
}

void LeaseCapacityAndSerial()
{
    auto budget = SessionTaskBudget::Create({1, 100, 0});
    std::vector<std::shared_ptr<TaskLease>> retained;
    for (unsigned index = 0; index < SessionTaskBudget::MaxLeases; ++index) {
        auto lease = budget->Acquire(0, TaskPriority::Deferred);
        assert(lease->Finish(0));
        retained.push_back(std::move(lease));
    }
    ExpectAcquireFailure(budget, 0, BudgetError::Capacity);
    retained.clear();
    VerifyEmpty(budget);

    auto stale = budget->Acquire(20, TaskPriority::Critical);
    budget->DropProcess(getpid());
    auto current = budget->Acquire(30, TaskPriority::Critical);
    assert(!stale->Finish(0) && stale->Bytes() == 0);
    stale.reset();
    assert(current->Bytes() == 30 && budget->Stats().active == 1);
    current.reset();
    VerifyEmpty(budget);
}

void DescriptorValidation()
{
    auto budget = SessionTaskBudget::Create({1, 100, 0});
    bool failed = false;
    try {
        SessionTaskBudget::Attach(budget->Fd(), getpid() + 1);
    } catch (const BudgetFailure &error) {
        assert(error.Code() == BudgetError::InvalidHandle);
        failed = true;
    }
    assert(failed);
    int invalid = open("/dev/null", O_RDWR | O_CLOEXEC);
    assert(invalid >= 0);
    failed = false;
    try {
        SessionTaskBudget::Attach(invalid);
    } catch (const BudgetFailure &error) {
        assert(error.Code() == BudgetError::InvalidHandle);
        failed = true;
    }
    close(invalid);
    assert(failed);

    Mapping mapping(budget->Fd());
    auto &shared = mapping.Get();
    const auto version = shared.version;
    shared.version += 1;
    failed = false;
    try {
        SessionTaskBudget::Attach(budget->Fd());
    } catch (const BudgetFailure &error) {
        assert(error.Code() == BudgetError::InvalidHandle);
        failed = true;
    }
    shared.version = version;
    assert(failed);
}

void ForkDoesNotReleaseParent()
{
    auto budget = SessionTaskBudget::Create({1, 100, 0});
    auto lease = budget->Acquire(60, TaskPriority::Critical);
    const auto child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(!lease->Finish(0));
        lease.reset();
        budget.reset();
        _exit(0);
    }
    WaitChild(child);
    assert(lease->Bytes() == 60 && budget->Stats().active == 1);
    lease.reset();
    VerifyEmpty(budget);
}

void KilledWorkerAndRobustRecovery()
{
    auto budget = SessionTaskBudget::Create({1, 100, 0});
    Pipe ready;
    const auto creator = getpid();
    const auto child = fork();
    assert(child >= 0);
    if (child == 0) {
        auto attached = SessionTaskBudget::Attach(budget->Fd(), creator);
        auto lease = attached->Acquire(60, TaskPriority::Critical);
        assert(lease && lease->Bytes() == 60);
        Mapping mapping(attached->Fd());
        auto &shared = mapping.Get();
        assert(pthread_mutex_lock(&shared.mutex) == 0);
        // Die during a state update with invalid derived counters. Recovery
        // must reconstruct them from the lease, not trust this partial state.
        shared.stats.active = 77;
        shared.stats.reserved_bytes = 999;
        Send(ready.write, 'r');
        for (;;) {
            pause();
        }
    }
    assert(Receive(ready.read) == 'r');
    assert(kill(child, SIGKILL) == 0);
    WaitChild(child, true);
    const auto recovered = budget->Stats();
    assert(recovered.active == 1 && recovered.reserved_bytes == 60 && recovered.recoveries == 1);
    budget->DropProcess(child);
    VerifyEmpty(budget);
    auto retry = budget->Acquire(90, TaskPriority::Resource);
    assert(retry && retry->Bytes() == 90);
    retry.reset();
    VerifyEmpty(budget);
}

void KilledRetainedOwnerWakesWaiter()
{
    auto budget = SessionTaskBudget::Create({1, 100, 0});
    Pipe ready;
    const auto creator = getpid();
    const auto child = fork();
    assert(child >= 0);
    if (child == 0) {
        auto attached = SessionTaskBudget::Attach(budget->Fd(), creator);
        auto lease = attached->Acquire(70, TaskPriority::Critical);
        assert(lease->Finish(70));
        Send(ready.write, 'r');
        auto blocked = attached->Acquire(10, TaskPriority::Resource);
        assert(blocked);
        Send(ready.write, 'a');
        for (;;) {
            pause();
        }
    }
    assert(Receive(ready.read) == 'r');
    assert(Receive(ready.read) == 'a');
    std::shared_ptr<TaskLease> waiting;
    std::thread worker(AcquireThread{budget, {}, &waiting});
    WaitWaiting(budget, 1);
    assert(kill(child, SIGKILL) == 0);
    WaitChild(child, true);
    budget->DropProcess(child);
    worker.join();
    assert(waiting && budget->Stats().retained_bytes == 0 && budget->Stats().active == 1);
    waiting.reset();
    VerifyEmpty(budget);
}

void KilledWaitingOwner()
{
    auto budget = SessionTaskBudget::Create({1, 100, 0});
    auto active = budget->Acquire(80, TaskPriority::Critical);
    const auto creator = getpid();
    const auto child = fork();
    assert(child >= 0);
    if (child == 0) {
        auto attached = SessionTaskBudget::Attach(budget->Fd(), creator);
        attached->Acquire(10, TaskPriority::Resource);
        _exit(3);
    }
    WaitWaiting(budget, 1);
    assert(kill(child, SIGKILL) == 0);
    WaitChild(child, true);
    budget->DropProcess(child);
    assert(budget->Stats().waiting == 0);

    std::shared_ptr<TaskLease> waiting;
    std::stop_source cancel;
    std::thread worker(AcquireThread{budget, cancel.get_token(), &waiting});
    WaitWaiting(budget, 1);
    cancel.request_stop();
    worker.join();
    assert(!waiting);
    active.reset();
    VerifyEmpty(budget);
}

struct ChildReservation {
    pid_t pid{};
    std::unique_ptr<Pipe> release;
    std::unique_ptr<Pipe> start;
};

ChildReservation SpawnReservation(const std::shared_ptr<SessionTaskBudget> &budget,
                                  TaskPriority priority, int ready, char identity,
                                  bool wait_to_start = false)
{
    ChildReservation child;
    child.release = std::make_unique<Pipe>();
    if (wait_to_start) {
        child.start = std::make_unique<Pipe>();
    }
    const auto creator = getpid();
    child.pid = fork();
    assert(child.pid >= 0);
    if (!child.pid) {
        if (child.start) {
            assert(Receive(child.start->read) == 's');
        }
        auto attached = SessionTaskBudget::Attach(budget->Fd(), creator);
        auto lease = attached->Acquire(10, priority);
        Send(ready, identity);
        assert(Receive(child.release->read) == 'q');
        lease.reset();
        _exit(0);
    }
    return child;
}

void PriorityAndOwnerFairness()
{
    auto budget = SessionTaskBudget::Create({1, 100, 0});
    auto active = budget->Acquire(10, TaskPriority::Critical);
    Pipe ready;
    auto deferred = SpawnReservation(budget, TaskPriority::Deferred, ready.write, 'd');
    WaitWaiting(budget, 1);
    auto resource = SpawnReservation(budget, TaskPriority::Resource, ready.write, 'r');
    WaitWaiting(budget, 2);
    auto critical = SpawnReservation(budget, TaskPriority::Critical, ready.write, 'c');
    WaitWaiting(budget, 3);
    active.reset();
    assert(Receive(ready.read) == 'c');
    Send(critical.release->write, 'q');
    WaitChild(critical.pid);
    assert(Receive(ready.read) == 'r');
    Send(resource.release->write, 'q');
    WaitChild(resource.pid);
    assert(Receive(ready.read) == 'd');
    Send(deferred.release->write, 'q');
    WaitChild(deferred.pid);
    VerifyEmpty(budget);

    active = budget->Acquire(10, TaskPriority::Critical);
    auto other = SpawnReservation(budget, TaskPriority::Critical, ready.write, 'o', true);
    std::shared_ptr<TaskLease> parent_result;
    std::thread parent_wait(AcquireThread{budget, {}, &parent_result, ready.write});
    WaitWaiting(budget, 1);
    Send(other.start->write, 's');
    WaitWaiting(budget, 2);
    assert(active->Finish(0));
    assert(Receive(ready.read) == 'o');
    Send(other.release->write, 'q');
    WaitChild(other.pid);
    assert(Receive(ready.read) == 'y');
    parent_wait.join();
    active.reset();
    parent_result.reset();
    VerifyEmpty(budget);
}
} // namespace

int main()
{
    DescriptorValidation();
    RetentionAndHeadroom();
    CancellationAndFinishWake();
    LeaseCapacityAndSerial();
    ForkDoesNotReleaseParent();
    KilledWorkerAndRobustRecovery();
    KilledRetainedOwnerWakesWaiter();
    KilledWaitingOwner();
    PriorityAndOwnerFairness();
}
