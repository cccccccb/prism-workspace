#include "fixtures/module_work_fixture.hpp"
#include "prism/launch/module.hpp"
#include "prism/runtime/session_task_budget.hpp"
#include "prism/runtime/task_scheduler.hpp"
#include "prism/sdk/module_session.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <memory>
#include <poll.h>
#include <string>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace std::chrono_literals;
using namespace module_work_fixture;
using prism::runtime::SessionTaskBudget;
using prism::runtime::TaskScheduler;
using prism::sdk::ModuleSession;
using prism::sdk::ModuleSessionLimits;

class Event {
public:
    Event() : fd_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK))
    {
        assert(fd_ >= 0);
    }

    ~Event()
    {
        close(fd_);
    }

    int Fd() const
    {
        return fd_;
    }

    void Signal() const
    {
        const std::uint64_t one = 1;
        assert(write(fd_, &one, sizeof(one)) == sizeof(one));
    }

    bool Ready() const
    {
        pollfd source{fd_, POLLIN, 0};
        assert(poll(&source, 1, 0) >= 0);
        return source.revents & POLLIN;
    }

    void Wait() const
    {
        pollfd source{fd_, POLLIN, 0};
        assert(poll(&source, 1, 5000) == 1 && (source.revents & POLLIN));
    }

private:
    int fd_;
};

template <typename Predicate> void Until(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

class Fixture {
public:
    explicit Fixture(const char *path) : handle_(dlopen(path, RTLD_NOW | RTLD_LOCAL))
    {
        assert(handle_);
        host_ = Symbol<decltype(host_)>("module_work_host");
        work_ = Symbol<decltype(work_)>("module_work_function");
        count_ = Symbol<decltype(count_)>("module_work_count");
        record_ = Symbol<decltype(record_)>("module_work_record");
        errors_ = Symbol<decltype(errors_)>("module_work_errors");
        reset_ = Symbol<decltype(reset_)>("module_work_reset");
        lifecycle_ = Symbol<decltype(lifecycle_)>("module_work_lifecycle");
        create_gate_ = Symbol<decltype(create_gate_)>("module_work_create_gate");
        completion_gate_ = Symbol<decltype(completion_gate_)>("module_work_completion_gate");
    }

    ~Fixture()
    {
        Close();
    }

    void Close()
    {
        if (handle_) {
            assert(dlclose(handle_) == 0);
            handle_ = nullptr;
        }
    }

    const PrismHostApiV1 *Host() const
    {
        return host_();
    }

    PrismWorkFunctionV1 Work() const
    {
        return work_();
    }

    std::size_t Count() const
    {
        return count_();
    }

    const Record &Result(std::size_t index) const
    {
        const auto *result = record_(index);
        assert(result);
        return *result;
    }

    unsigned Errors() const
    {
        return errors_();
    }

    void Reset() const
    {
        reset_();
    }

    void Lifecycle(int fd) const
    {
        lifecycle_(fd);
    }

    void CreateGate(int entered, int gate) const
    {
        create_gate_(entered, gate);
    }

    void CompletionGate(int entered, int gate) const
    {
        completion_gate_(entered, gate);
    }

private:
    template <typename Function> Function Symbol(const char *name)
    {
        auto function = reinterpret_cast<Function>(dlsym(handle_, name));
        assert(function);
        return function;
    }

    void *handle_;
    const PrismHostApiV1 *(*host_)();
    PrismWorkFunctionV1 (*work_)();
    std::size_t (*count_)();
    const Record *(*record_)(std::size_t);
    unsigned (*errors_)();
    void (*reset_)();
    void (*lifecycle_)(int);
    void (*create_gate_)(int, int);
    void (*completion_gate_)(int, int);
};

struct Sinks {
    const std::thread::id owner{std::this_thread::get_id()};
    unsigned bindings{}, launches{}, subscriptions{}, themes{}, schemes{};

    bool Binding(std::string_view key, prism::runtime::PropertyValue value)
    {
        assert(std::this_thread::get_id() == owner && key == "completed");
        assert(std::holds_alternative<double>(value));
        ++bindings;
        return true;
    }

    std::uint64_t Launch(std::string_view)
    {
        assert(std::this_thread::get_id() == owner);
        ++launches;
        return 11;
    }

    std::uint64_t Subscribe()
    {
        assert(std::this_thread::get_id() == owner);
        ++subscriptions;
        return 12;
    }

    std::uint64_t Theme(std::string_view)
    {
        assert(std::this_thread::get_id() == owner);
        ++themes;
        return 13;
    }

    std::uint64_t Scheme(std::string_view)
    {
        assert(std::this_thread::get_id() == owner);
        ++schemes;
        return 14;
    }
};

std::unique_ptr<ModuleSession> Session(const char *file, Sinks &sinks,
                                       std::shared_ptr<TaskScheduler> scheduler = {},
                                       ModuleSessionLimits limits = {})
{
    return std::make_unique<ModuleSession>(
        file, "work.test", 1, std::bind_front(&Sinks::Binding, &sinks),
        std::bind_front(&Sinks::Launch, &sinks), std::bind_front(&Sinks::Subscribe, &sinks),
        std::bind_front(&Sinks::Theme, &sinks), std::bind_front(&Sinks::Scheme, &sinks),
        std::move(scheduler), limits);
}

std::vector<std::uint8_t> Bytes(Input input, std::vector<std::uint8_t> payload = {})
{
    std::vector<std::uint8_t> bytes(sizeof(input) + payload.size());
    std::memcpy(bytes.data(), &input, sizeof(input));
    if (!payload.empty()) {
        std::memcpy(bytes.data() + sizeof(input), payload.data(), payload.size());
    }
    return bytes;
}

int Submit(const PrismHostApiV1 *host, Fixture &fixture, std::uint64_t id,
           const std::vector<std::uint8_t> &bytes, std::size_t result_limit = 128,
           std::uint32_t priority = PRISM_WORK_CRITICAL_V1)
{
    PrismWorkRequestV1 request{sizeof(request), id,           priority,
                               128 * 1024,      result_limit, {bytes.data(), bytes.size()},
                               fixture.Work()};
    return host->submit_work(host->context, &request);
}

void WaitCompletion(ModuleSession &session)
{
    pollfd source{session.WorkCompletionFd(), POLLIN, 0};
    assert(source.fd >= 0 && poll(&source, 1, 5000) == 1 && (source.revents & POLLIN));
}

void DispatchUntil(ModuleSession &session, Fixture &fixture, std::size_t count)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (fixture.Count() < count) {
        assert(std::chrono::steady_clock::now() < deadline);
        WaitCompletion(session);
        assert(session.DispatchWork() > 0);
    }
}

std::shared_ptr<SessionTaskBudget> Budget()
{
    return SessionTaskBudget::Create({2, 1024 * 1024, 256 * 1024});
}

void CopyAndOwnerCallbacks(const char *file, Fixture &fixture)
{
    fixture.Reset();
    Sinks sinks;
    auto budget = Budget();
    auto scheduler = std::make_shared<TaskScheduler>(budget);
    auto session = Session(file, sinks, scheduler);
    assert(session->Start() && !session->BackendReady());
    const auto *host = fixture.Host();
    const auto forbidden = Bytes({Mode::Forbidden});
    assert(Submit(host, fixture, 2, forbidden) == PRISM_WORK_ACCEPTED_V1);
    WaitCompletion(*session);
    assert(!session->BackendReady() && sinks.bindings == 0 && sinks.launches == 0 &&
           sinks.subscriptions == 0 && sinks.themes == 0 && sinks.schemes == 0);
    assert(session->TimeoutMs(prism::sdk::MonotonicNs(), -1) == -1);
    DispatchUntil(*session, fixture, 1);
    const auto &rejected = fixture.Result(0);
    std::uint16_t mask{};
    assert(rejected.status == PRISM_WORK_SUCCEEDED_V1 && rejected.size == sizeof(mask));
    std::memcpy(&mask, rejected.bytes, sizeof(mask));
    assert(mask == 0x1ff && session->BackendReady() && sinks.bindings == 1);

    Event entered, gate;
    const std::vector<std::uint8_t> expected{0x12, 0x00, 0xff, 0x7e};
    auto bytes = Bytes({Mode::Echo, entered.Fd(), gate.Fd()}, expected);
    assert(Submit(host, fixture, 1, bytes) == PRISM_WORK_ACCEPTED_V1);
    entered.Wait();
    std::fill(bytes.begin(), bytes.end(), 0xa5);
    assert(session->WorkPending() && session->BackendReady() && sinks.bindings == 1);
    gate.Signal();
    WaitCompletion(*session);
    assert(budget->Stats().retained_bytes > 0 && fixture.Count() == 1);
    DispatchUntil(*session, fixture, 2);
    const auto &result = fixture.Result(1);
    assert(result.task == 1 && result.status == PRISM_WORK_SUCCEEDED_V1 && result.error == 0 &&
           result.size == expected.size());
    assert(std::equal(expected.begin(), expected.end(), result.bytes));
    assert(session->BackendReady() && sinks.bindings == 2 && !session->WorkPending());
    assert(budget->Stats().retained_bytes == 0 && fixture.Errors() == 0);
    assert(mask == 0x1ff && sinks.bindings == 2 && sinks.launches == 0 &&
           sinks.subscriptions == 0 && sinks.themes == 0 && sinks.schemes == 0);
    assert(session->TimeoutMs(prism::sdk::MonotonicNs(), -1) == -1 && fixture.Errors() == 0);
}

void ResultsAndValidation(const char *file, Fixture &fixture)
{
    fixture.Reset();
    Sinks sinks;
    auto session = Session(file, sinks);
    assert(session->Start());
    const auto *host = fixture.Host();
    const std::array<Mode, 4> modes{Mode::Empty, Mode::Overflow, Mode::RepeatedResult,
                                    Mode::Failure};
    for (std::size_t index = 0; index < modes.size(); ++index) {
        const auto input = Bytes({modes[index]}, {1, 0, 2, 3, 4});
        assert(Submit(host, fixture, 20 + index, input, modes[index] == Mode::Overflow ? 4 : 128) ==
               PRISM_WORK_ACCEPTED_V1);
        DispatchUntil(*session, fixture, index + 1);
        const auto &result = fixture.Result(index);
        assert(result.task == 20 + index && result.size == 0);
        assert(result.status == (index == 0 ? PRISM_WORK_SUCCEEDED_V1 : PRISM_WORK_FAILED_V1));
        assert(result.error == (index == 0 ? 0 : index == 3 ? 17 : -1));
    }
    const auto input = Bytes({Mode::Empty});
    assert(Submit(host, fixture, 0, input) == PRISM_WORK_INVALID_V1);
    assert(Submit(host, fixture, 30, input, 128, 1) == PRISM_WORK_INVALID_V1);
    PrismWorkRequestV1 short_request{};
    short_request.struct_size = offsetof(PrismWorkRequestV1, work);
    assert(host->submit_work(host->context, &short_request) == PRISM_WORK_INVALID_V1);
    assert(!session->WorkPending() && fixture.Errors() == 0);
}

void CancelActiveAndCompleted(const char *file, Fixture &fixture)
{
    fixture.Reset();
    Sinks sinks;
    auto session = Session(file, sinks);
    assert(session->Start());
    const auto *host = fixture.Host();
    Event entered, gate;
    auto input = Bytes({Mode::Late, entered.Fd(), gate.Fd()}, {0x51, 0});
    assert(Submit(host, fixture, 41, input) == PRISM_WORK_ACCEPTED_V1);
    entered.Wait();
    assert(host->cancel_work(host->context, 41) == 0);
    assert(Submit(host, fixture, 41, input) == PRISM_WORK_BUSY_V1);
    DispatchUntil(*session, fixture, 1);
    assert(fixture.Result(0).status == PRISM_WORK_CANCELLED_V1 && fixture.Result(0).size == 0);

    input = Bytes({Mode::Echo}, {0x23});
    assert(Submit(host, fixture, 41, input) == PRISM_WORK_ACCEPTED_V1);
    WaitCompletion(*session);
    assert(session->WorkPending());
    assert(host->cancel_work(host->context, 41) == 0);
    assert(Submit(host, fixture, 41, input) == PRISM_WORK_BUSY_V1);
    assert(fixture.Count() == 1);
    DispatchUntil(*session, fixture, 2);
    assert(fixture.Result(1).status == PRISM_WORK_CANCELLED_V1 && fixture.Result(1).size == 0);
    assert(Submit(host, fixture, 41, input) == PRISM_WORK_ACCEPTED_V1);
    DispatchUntil(*session, fixture, 3);
    assert(fixture.Result(2).status == PRISM_WORK_SUCCEEDED_V1 && fixture.Result(2).size == 1);
    assert(!session->WorkPending() && fixture.Errors() == 0);
}

void SharedQuotaAndQueuedCancel(const char *file, Fixture &fixture)
{
    fixture.Reset();
    Sinks first_sinks, second_sinks;
    auto budget = Budget();
    auto scheduler = std::make_shared<TaskScheduler>(budget, 2);
    ModuleSessionLimits limits;
    limits.completions_per_turn = 1;
    auto first = Session(file, first_sinks, scheduler, limits);
    auto second = Session(file, second_sinks, scheduler);
    assert(first->Start());
    const auto *first_host = fixture.Host();
    assert(second->Start());
    const auto *second_host = fixture.Host();
    Event first_entered, first_gate, second_entered, second_gate, queued_entered, queued_gate;
    assert(Submit(first_host, fixture, 101,
                  Bytes({Mode::Echo, first_entered.Fd(), first_gate.Fd()}, {1})) ==
           PRISM_WORK_ACCEPTED_V1);
    assert(Submit(second_host, fixture, 202,
                  Bytes({Mode::Echo, second_entered.Fd(), second_gate.Fd()}, {2})) ==
           PRISM_WORK_ACCEPTED_V1);
    first_entered.Wait();
    second_entered.Wait();
    assert(budget->Stats().active == 2);
    const auto queued = Bytes({Mode::Echo, queued_entered.Fd(), queued_gate.Fd()}, {3});
    assert(Submit(first_host, fixture, 303, queued) == PRISM_WORK_ACCEPTED_V1);
    Until([&scheduler] { return scheduler->Stats().queued == 1; });
    assert(budget->Stats().active == 2 && !queued_entered.Ready());
    assert(first_host->cancel_work(first_host->context, 303) == 0);
    assert(Submit(first_host, fixture, 303, queued) == PRISM_WORK_BUSY_V1);
    // Both supported workers are occupied. A queued cancellation is consumed
    // only after one active task releases a worker; it must never invoke Work.
    first_gate.Signal();
    DispatchUntil(*first, fixture, 2);
    assert(fixture.Result(0).task == 101 && fixture.Result(0).status == PRISM_WORK_SUCCEEDED_V1);
    assert(fixture.Result(1).task == 303 && fixture.Result(1).status == PRISM_WORK_CANCELLED_V1);
    assert(!queued_entered.Ready() && budget->Stats().active == 1);
    second_gate.Signal();
    WaitCompletion(*second);
    assert(second->DispatchWork() == 1);
    assert(budget->Stats().active == 0 && budget->Stats().retained_bytes == 0 &&
           fixture.Count() == 3 && fixture.Errors() == 0);
}

struct DelayedRelease {
    const Event *entered, *gate;
    std::chrono::nanoseconds delay{8ms};

    void operator()() const
    {
        entered->Wait();
        const int timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        assert(timer >= 0);
        itimerspec spec{};
        spec.it_value.tv_nsec = delay.count();
        assert(timerfd_settime(timer, 0, &spec, nullptr) == 0);
        pollfd source{timer, POLLIN, 0};
        assert(poll(&source, 1, 5000) == 1 && (source.revents & POLLIN));
        close(timer);
        gate->Signal();
    }
};

void DispatchBounds(const char *file, Fixture &fixture, bool timed)
{
    fixture.Reset();
    Sinks sinks;
    auto budget = Budget();
    auto scheduler = std::make_shared<TaskScheduler>(budget);
    ModuleSessionLimits limits;
    limits.completions_per_turn = timed ? 8 : 1;
    limits.dispatch_budget = timed ? 1ms : 100ms;
    auto session = Session(file, sinks, scheduler, limits);
    assert(session->Start());
    const auto *host = fixture.Host();
    const auto bytes = Bytes({Mode::Empty});
    assert(Submit(host, fixture, 501, bytes) == PRISM_WORK_ACCEPTED_V1);
    assert(Submit(host, fixture, 502, bytes) == PRISM_WORK_ACCEPTED_V1);
    Until([&scheduler] { return scheduler->Stats().completed == 2; });
    assert(budget->Stats().retained_leases == 2);
    Event entered, gate;
    std::thread release;
    if (timed) {
        fixture.CompletionGate(entered.Fd(), gate.Fd());
        release = std::thread(DelayedRelease{&entered, &gate});
    }
    assert(session->DispatchWork() == 1 && fixture.Count() == 1 && session->WorkPending());
    if (release.joinable()) {
        release.join();
    }
    // The remaining retained result must stay immediately discoverable without
    // a new worker completion, and must wake the next owner dispatch.
    WaitCompletion(*session);
    assert(budget->Stats().retained_leases == 1);
    assert(session->DispatchWork() == 1 && fixture.Count() == 2 && !session->WorkPending());
    assert(budget->Stats().retained_leases == 0 && fixture.Errors() == 0);
}

void StopThenDestroyThenUnload(const char *file, bool explicit_stop)
{
    Fixture fixture(file);
    fixture.Reset();
    int pipe_fds[2];
    assert(pipe(pipe_fds) == 0);
    fixture.Lifecycle(pipe_fds[1]);
    Sinks sinks;
    auto session = Session(file, sinks);
    assert(session->Start());
    const auto *host = fixture.Host();
    Event entered, gate;
    const auto input = Bytes({Mode::Late, entered.Fd(), gate.Fd(), pipe_fds[1]}, {0x32});
    assert(Submit(host, fixture, 601, input) == PRISM_WORK_ACCEPTED_V1);
    entered.Wait();
    if (explicit_stop) {
        session->StopWork();
        assert(!session->WorkPending() && session->DispatchWork() == 0 && fixture.Count() == 0);
        assert(Submit(host, fixture, 602, input) == PRISM_WORK_CLOSED_V1);
        assert(host->backend_ready(host->context) != 0 && sinks.bindings == 0);
    }
    session.reset();
    assert(fixture.Errors() == 0 && fixture.Count() == 0 && sinks.bindings == 0);
    fixture.Close();
    std::array<char, 3> sequence;
    std::size_t read_bytes = 0;
    while (read_bytes < sequence.size()) {
        pollfd source{pipe_fds[0], POLLIN, 0};
        assert(poll(&source, 1, 5000) == 1 && (source.revents & POLLIN));
        const auto count =
            read(pipe_fds[0], sequence.data() + read_bytes, sequence.size() - read_bytes);
        assert(count > 0);
        read_bytes += static_cast<std::size_t>(count);
    }
    assert((sequence == std::array<char, 3>{'E', 'D', 'U'}));
    close(pipe_fds[0]);
    close(pipe_fds[1]);
}

void EntryBudgets(const char *file, const char *load_file, Fixture &fixture)
{
    fixture.Reset();
    Sinks sinks;
    Event entered, gate;
    fixture.CreateGate(entered.Fd(), gate.Fd());
    std::thread release(DelayedRelease{&entered, &gate, 20ms});
    ModuleSessionLimits limits;
    limits.entry_budget = 10ms;
    auto session = Session(file, sinks, {}, limits);
    assert(!session->Start() && !session->BackendReady() && !session->StartDiagnostic().empty());
    release.join();
    assert(!session->WorkPending() && fixture.Count() == 0 && fixture.Errors() == 0);
    session.reset();
    fixture.Reset();

    Event load_entered, load_gate;
    const auto entered_value = std::to_string(load_entered.Fd());
    const auto gate_value = std::to_string(load_gate.Fd());
    assert(setenv("PRISM_WORK_TEST_LOAD_ENTER", entered_value.c_str(), 1) == 0);
    assert(setenv("PRISM_WORK_TEST_LOAD_GATE", gate_value.c_str(), 1) == 0);
    std::thread load_release(DelayedRelease{&load_entered, &load_gate, 20ms});
    auto slow = Session(load_file, sinks, {}, limits);
    assert(!slow->Start() && !slow->BackendReady() && !slow->StartDiagnostic().empty());
    load_release.join();
    assert(!slow->WorkPending());
    assert(unsetenv("PRISM_WORK_TEST_LOAD_ENTER") == 0);
    assert(unsetenv("PRISM_WORK_TEST_LOAD_GATE") == 0);
}

void PartialAbi(const char *file)
{
    prism::launch::AppModule module(file);
    assert(module.Api().create && module.Api().destroy && !module.Api().on_work_completed);
    PrismHostApiV1 host{};
    PrismAppInitV1 init{sizeof(init), PRISM_APP_ABI_V1, 1, {"partial", 7}, &host};
    assert(module.Api().create(&init) == &host);
    module.Api().destroy(&host);
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 4);
    {
        Fixture fixture(argv[1]);
        CopyAndOwnerCallbacks(argv[1], fixture);
        ResultsAndValidation(argv[1], fixture);
        CancelActiveAndCompleted(argv[1], fixture);
        SharedQuotaAndQueuedCancel(argv[1], fixture);
        DispatchBounds(argv[1], fixture, false);
        DispatchBounds(argv[1], fixture, true);
        EntryBudgets(argv[1], argv[2], fixture);
    }
    StopThenDestroyThenUnload(argv[1], true);
    StopThenDestroyThenUnload(argv[1], false);
    PartialAbi(argv[3]);
}
