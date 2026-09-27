#include "prism/sdk/app_host.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using Host = prism::sdk::AppHost;
using UiState = prism::sdk::HostUiState;
using prism::contracts::LaunchEvent;
using prism::contracts::LaunchMilestone;
using Clock = std::chrono::steady_clock;

enum EventKind : std::uint32_t {
    Created = 1,
    Entered,
    Finished,
    Completed,
    ThemeReceived,
    LaunchReceived,
    InstanceReceived,
    Cancelled,
    Destroyed,
    Unloaded
};

struct Event {
    std::uint32_t kind{}, task{}, status{}, active{};
    std::int32_t error{};
    std::uint64_t thread{};
};

void Require(bool condition, std::string_view detail)
{
    if (!condition) {
        throw std::runtime_error(std::string(detail));
    }
}

class Descriptors {
public:
    Descriptors()
    {
        Require(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, events_) == 0,
                "Cannot create fixture message pair");
        for (auto &gate : gates_) {
            gate = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            Require(gate >= 0, "Cannot create work gate");
        }
        control_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        deadline_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        Require(control_ >= 0 && deadline_ >= 0, "Cannot create caller/deadline sources");
        itimerspec limit{};
        limit.it_value.tv_sec = 9;
        Require(timerfd_settime(deadline_, 0, &limit, nullptr) == 0, "Cannot arm test timeout");
        Environment("PRISM_TEST_BUSINESS_EVENTS", events_[1]);
        Environment("PRISM_TEST_BUSINESS_GATE1", gates_[0]);
        Environment("PRISM_TEST_BUSINESS_GATE2", gates_[1]);
    }

    ~Descriptors()
    {
        for (int fd : {events_[0], events_[1], gates_[0], gates_[1], control_, deadline_}) {
            if (fd >= 0) {
                close(fd);
            }
        }
    }

    int Events() const
    {
        return events_[0];
    }

    int Control() const
    {
        return control_;
    }

    int Deadline() const
    {
        return deadline_;
    }

    void Release(unsigned task)
    {
        Signal(gates_[task - 1]);
    }

    void SignalControl()
    {
        Signal(control_);
    }

    void DrainControl()
    {
        std::uint64_t count;
        Require(read(control_, &count, sizeof(count)) == sizeof(count), "Caller readiness lost");
    }

    void Read()
    {
        for (;;) {
            Event event;
            const auto size = recv(events_[0], &event, sizeof(event), MSG_DONTWAIT);
            if (size < 0 && errno == EINTR) {
                continue;
            }
            if (size < 0 && errno == EAGAIN) {
                return;
            }
            Require(size == sizeof(event), "Invalid fixture message size");
            records.push_back(event);
        }
    }

    unsigned Count(unsigned kind) const
    {
        return std::count_if(records.begin(), records.end(),
                             [kind](const Event &event) { return event.kind == kind; });
    }

    const Event &For(unsigned kind, unsigned task) const
    {
        const auto found =
            std::find_if(records.begin(), records.end(), [kind, task](const Event &event) {
                return event.kind == kind && event.task == task;
            });
        Require(found != records.end(), "Missing fixture task event");
        return *found;
    }

    std::vector<Event> records;

private:
    static void Environment(const char *name, int fd)
    {
        Require(setenv(name, std::to_string(fd).c_str(), 1) == 0, "Cannot configure test fixture");
    }

    static void Signal(int fd)
    {
        const std::uint64_t one = 1;
        ssize_t size;
        do {
            size = write(fd, &one, sizeof(one));
        } while (size < 0 && errno == EINTR);
        Require(size == sizeof(one), "Cannot release fixture gate");
    }

    int events_[2]{-1, -1}, gates_[2]{-1, -1};
    int control_{-1}, deadline_{-1};
};

struct Recorder {
    Host *host{};
    std::uint64_t owner{static_cast<std::uint64_t>(syscall(SYS_gettid))};
    std::vector<LaunchEvent> events;

    void Record(const LaunchEvent &event)
    {
        Require(static_cast<std::uint64_t>(syscall(SYS_gettid)) == owner,
                "Launch event escaped owner thread");
        if (event.milestone == LaunchMilestone::BackendReady) {
            Require(host->GetUiState().master_presented,
                    "Backend became ready before held-work Master feedback");
        }
        events.push_back(event);
    }

    unsigned Count(LaunchMilestone milestone) const
    {
        return std::count_if(events.begin(), events.end(), [milestone](const LaunchEvent &event) {
            return event.milestone == milestone;
        });
    }
};

prism::contracts::ThemeSnapshot Theme(std::uint64_t generation)
{
    prism::contracts::ThemeSnapshot theme;
    theme.id = "business-probe";
    theme.name = "Async business fixture";
    theme.generation = generation;
    theme.colors = {{"foreground", generation == 1 ? prism::contracts::Color{230, 240, 250, 255}
                                                   : prism::contracts::Color{180, 215, 245, 255}}};
    return theme;
}

std::uint64_t RegisterLaunch(std::string_view id)
{
    Require(id == "business-other", "Fixture requested unexpected app");
    return 9;
}

std::uint64_t RegisterInstances()
{
    return 9;
}

void Pump(Host &host, Descriptors &fds, int extra = -1, int timeout = -1)
{
    std::vector<pollfd> sources{{fds.Deadline(), POLLIN, 0}};
    if (extra >= 0) {
        sources.push_back({extra, POLLIN, 0});
    }
    Require(host.Pump(timeout, sources), "Host stopped during async business gate");
    Require(!sources.front().revents, "Native business gate timeout");
    if (extra == fds.Control()) {
        Require(sources.back().revents & POLLIN, "Host consumed or lost caller control readiness");
    }
}

void WaitPresentation(Host &host, Descriptors &fds, std::uint64_t prior)
{
    while (host.GetUiState().master_presented_submission <= prior) {
        Pump(host, fds);
    }
}

void WaitBlocked(Host &host, Descriptors &fds, Recorder &recorder)
{
    while (!host.GetUiState().master_presented) {
        Pump(host, fds);
    }
    fds.Read();
    while (fds.Count(Entered) != 2) {
        Pump(host, fds, fds.Events());
        fds.Read();
    }
    Require(fds.Count(Completed) == 0 && recorder.Count(LaunchMilestone::BackendReady) == 0,
            "Blocked work already completed or became Ready");
    const auto &one = fds.For(Entered, 1);
    const auto &two = fds.For(Entered, 2);
    Require(one.thread != recorder.owner && two.thread != recorder.owner &&
                one.thread != two.thread,
            "Two work units did not run on distinct background workers");
    Require(std::max(one.active, two.active) == 2, "Work gates did not overlap");
    Require(host.GetUiState().business_work_pending, "Pending work not reflected in Host state");
    const auto first =
        std::find_if(recorder.events.begin(), recorder.events.end(), [](const LaunchEvent &event) {
            return event.milestone == LaunchMilestone::FirstPresented;
        });
    Require(first != recorder.events.end() && first->detail.find("V3D") != std::string::npos,
            "Native business gate did not present through V3D");
}

void Controls(Host &host, Descriptors &fds, Recorder &recorder)
{
    fds.SignalControl();
    Pump(host, fds, fds.Control());
    fds.DrainControl();
    auto prior = host.GetUiState().master_presented_submission;
    std::string diagnostic;
    Require(host.ApplyTheme(Theme(2), &diagnostic) && diagnostic.empty(),
            "Theme blocked behind business work");
    host.DeliverThemeEvent(
        {0, 2, prism::contracts::ThemeStatus::Applied, "business-probe", "Probe", {}, "dark"});
    WaitPresentation(host, fds, prior);
    prior = host.GetUiState().master_presented_submission;
    host.DeliverLaunchEvent({{9}, {10}, 1, LaunchMilestone::Activated});
    WaitPresentation(host, fds, prior);
    prior = host.GetUiState().master_presented_submission;
    host.DeliverInstanceEvent(
        {{9}, {10}, 1, prism::contracts::InstanceChange::Running, "business-other"});
    WaitPresentation(host, fds, prior);
    fds.Read();
    Require(fds.Count(ThemeReceived) >= 2 && fds.Count(LaunchReceived) == 1 &&
                fds.Count(InstanceReceived) == 1,
            "Business control callbacks were not projected while work was pending");
    Require(fds.Count(Completed) == 0 && recorder.Count(LaunchMilestone::BackendReady) == 0,
            "Controls incorrectly completed held work");
}

void WaitCompleted(Host &host, Descriptors &fds, unsigned count)
{
    fds.Read();
    while (fds.Count(Completed) < count) {
        // Fixture events are intentionally NOT in this wait set: only the Host's
        // own completion FD, Wayland, and a one-shot test timeout can wake it.
        Pump(host, fds);
        fds.Read();
    }
}

struct CaseReport {
    std::string name;
    bool passed{}, parallel{}, controls{}, completion_fd{}, caller_priority{},
        joined_before_destroy{};
    unsigned completed{}, cancelled{}, failed{};
    std::uint64_t first_submission{}, presented_submission{}, close_us{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CaseReport, name, passed, parallel, controls, completion_fd,
                                   caller_priority, joined_before_destroy, completed, cancelled,
                                   failed, first_submission, presented_submission, close_us)

CaseReport Run(const std::string &socket, const std::filesystem::path &package, std::string name)
{
    Descriptors fds;
    Recorder recorder;
    prism::sdk::HostConfig config;
    config.socket = socket;
    config.initial_theme = Theme(1);
    config.on_event = std::bind_front(&Recorder::Record, &recorder);
    config.launch_app = RegisterLaunch;
    config.subscribe_instances = RegisterInstances;
    Host host(std::move(config));
    recorder.host = &host;
    Require(host.Bind(prism::launch::LoadPackage(package)), "Native business Bind failed");
    WaitBlocked(host, fds, recorder);
    Controls(host, fds, recorder);
    CaseReport report;
    report.name = std::move(name);
    report.parallel = report.controls = true;
    report.first_submission = host.GetUiState().master_first_submission;
    report.presented_submission = host.GetUiState().master_presented_submission;
    if (report.name == "close-cancel") {
        const auto start = Clock::now();
        host.Close();
        report.close_us =
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
        Require(report.close_us < 5000000, "Close did not cancel blocked work promptly");
        fds.Read();
        Require(fds.Count(Cancelled) == 2 && fds.Count(Completed) == 0,
                "Close failed cooperative cancellation or delivered a late completion");
    } else {
        auto prior = host.GetUiState().master_presented_submission;
        fds.Release(1);
        WaitCompleted(host, fds, 1);
        WaitPresentation(host, fds, prior);
        Require(recorder.Count(LaunchMilestone::BackendReady) == 0,
                "Ready preceded the second work result");
        fds.SignalControl();
        fds.Release(2);
        while (!host.GetUiState().business_work_completion_ready) {
            Pump(host, fds, fds.Control(), 0);
            fds.Read();
            Require(fds.Count(Completed) == 1 && recorder.Count(LaunchMilestone::BackendReady) == 0,
                    "Completion ran before caller control was handled");
        }
        Require(fds.Count(Completed) == 1, "Published completion bypassed caller priority");
        report.caller_priority = true;
        fds.DrainControl();
        prior = host.GetUiState().master_presented_submission;
        WaitCompleted(host, fds, 2);
        while (!recorder.Count(LaunchMilestone::BackendReady)) {
            Pump(host, fds);
        }
        WaitPresentation(host, fds, prior);
        Require(recorder.Count(LaunchMilestone::BackendReady) == 1 &&
                    recorder.Count(LaunchMilestone::Failed) == 0,
                "Incorrect business readiness/failure milestones");
        const auto &two = fds.For(Completed, 2);
        Require(two.thread == recorder.owner, "Result handler escaped UI owner");
        if (report.name == "typed-failure") {
            Require(two.status == 2 && two.error == 73, "Failure lost typed status/error");
            report.failed = 1;
        } else {
            Require(two.status == 0 && two.error == 0, "Success completion became a failure");
        }
        report.completion_fd = true;
        const auto ui = host.GetUiState();
        report.first_submission = ui.master_first_submission;
        report.presented_submission = ui.master_presented_submission;
        Require(ui.master_presented_submission >= ui.master_first_submission &&
                    ui.master_first_submission,
                "Business feedback lost installed Master submission identity");
        host.Close();
        fds.Read();
    }
    Require(fds.Count(Destroyed) == 1 && fds.Count(Unloaded) == 1,
            "Joined module was not destroyed/unloaded exactly once");
    const auto destroyed = std::find_if(fds.records.begin(), fds.records.end(),
                                        [](const Event &event) { return event.kind == Destroyed; });
    Require(std::none_of(destroyed, fds.records.end(),
                         [](const Event &event) {
                             return event.kind == Completed || event.kind == Finished ||
                                    event.kind == Cancelled;
                         }),
            "Work/result notification followed module destruction");
    report.completed = fds.Count(Completed);
    report.cancelled = fds.Count(Cancelled);
    report.joined_before_destroy = report.passed = true;
    return report;
}

struct Report {
    unsigned schema_version{1};
    std::string gate{"native-async-business"};
    bool passed{};
    std::string limitation{"CPU work is asynchronous; dlopen/create/static constructors remain "
                           "owner-thread entrypoints. No pointer action or resize gate."};
    std::vector<CaseReport> scenarios;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Report, schema_version, gate, passed, limitation, scenarios)
} // namespace

int main(int argc, char **argv)
{
    try {
        Require(argc == 4, "Usage: async_business_probe <socket> <packages> <report.json>");
        Report report;
        const std::filesystem::path root(argv[2]);
        report.scenarios.push_back(Run(argv[1], root / "success", "success"));
        report.scenarios.push_back(Run(argv[1], root / "failure", "typed-failure"));
        report.scenarios.push_back(Run(argv[1], root / "cancel", "close-cancel"));
        report.passed = true;
        std::ofstream output(argv[3]);
        Require(bool(output), "Cannot write native evidence");
        output << nlohmann::json(report).dump(2) << '\n';
        std::cout << "Native asynchronous business: 3 scenarios passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "async_business_probe: " << error.what() << '\n';
        return 1;
    }
}
