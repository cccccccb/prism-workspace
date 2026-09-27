#include "prism/runtime/session_task_budget.hpp"
#include "prism/sdk/app_host.hpp"
#include "prism/theme/compiler.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using Host = prism::sdk::AppHost;
using Startup = prism::sdk::HostStartupStats;
using Theme = prism::contracts::ThemeSnapshot;
using Clock = std::chrono::steady_clock;

std::uint64_t Now()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
        .count();
}

void Require(bool condition, std::string_view detail)
{
    if (!condition) {
        throw std::runtime_error(std::string(detail));
    }
}

class Sources {
public:
    Sources()
    {
        guard_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        Require(guard_ >= 0, "Cannot create one-shot deadline");
        control_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (control_ < 0) {
            close(guard_);
            throw std::runtime_error("Cannot create control source");
        }
        itimerspec deadline{};
        deadline.it_value.tv_sec = 8;
        if (timerfd_settime(guard_, 0, &deadline, nullptr) != 0) {
            close(control_);
            close(guard_);
            throw std::runtime_error("Cannot arm one-shot deadline");
        }
    }

    ~Sources()
    {
        close(control_);
        close(guard_);
    }

    Sources(const Sources &) = delete;
    Sources &operator=(const Sources &) = delete;

    int Guard() const
    {
        return guard_;
    }

    int Control() const
    {
        return control_;
    }

    void Signal()
    {
        const std::uint64_t one = 1;
        ssize_t size;
        do {
            size = write(control_, &one, sizeof(one));
        } while (size < 0 && errno == EINTR);
        Require(size == sizeof(one), "Cannot signal caller control");
    }

    void Drain()
    {
        std::uint64_t value;
        ssize_t size;
        do {
            size = read(control_, &value, sizeof(value));
        } while (size < 0 && errno == EINTR);
        Require(size == sizeof(value), "Host consumed caller-owned readiness");
    }

private:
    int guard_{-1};
    int control_{-1};
};

struct MemoryPoint {
    std::uint64_t observed_ns{}, pss_kib{}, rss_kib{}, sampling_us{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MemoryPoint, observed_ns, pss_kib, rss_kib, sampling_us)

MemoryPoint Memory()
{
    MemoryPoint result;
    const auto start = Now();
    result.observed_ns = start;
    std::ifstream stream("/proc/self/smaps_rollup");
    std::string line;
    while (std::getline(stream, line)) {
        if (line.starts_with("Pss:") || line.starts_with("Rss:")) {
            const auto value = std::stoull(line.substr(4));
            if (line.starts_with("Pss:")) {
                result.pss_kib = value;
            } else {
                result.rss_kib = value;
            }
        }
    }
    result.sampling_us = (Now() - start) / 1000;
    return result;
}

struct Milestones {
    std::uint64_t bind_ns{}, master_queued_ns{}, preview_submitted_ns{}, preview_presented_ns{},
        master_prepared_ns{}, master_installed_ns{}, master_submitted_ns{}, master_presented_ns{},
        backend_ready_ns{}, deferred_complete_ns{};
    std::uint64_t frontend_prepare_us{}, preview_prepare_us{}, module_load_us{}, module_create_us{},
        pump_processing_count{}, pump_processing_total_us{}, pump_processing_max_us{};
    std::uint64_t egl_init_us{}, ganesh_init_us{}, first_submit_build_us{}, first_render_us{},
        first_swap_us{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Milestones, bind_ns, master_queued_ns, preview_submitted_ns,
                                   preview_presented_ns, master_prepared_ns, master_installed_ns,
                                   master_submitted_ns, master_presented_ns, backend_ready_ns,
                                   deferred_complete_ns, frontend_prepare_us, preview_prepare_us,
                                   module_load_us, module_create_us, pump_processing_count,
                                   pump_processing_total_us, pump_processing_max_us, egl_init_us,
                                   ganesh_init_us, first_submit_build_us, first_render_us,
                                   first_swap_us)

Milestones Observe(const Startup &stats)
{
    return {stats.bind_ns,
            stats.master_queued_ns,
            stats.preview_submitted_ns,
            stats.preview_presented_ns,
            stats.master_prepared_ns,
            stats.master_installed_ns,
            stats.master_submitted_ns,
            stats.master_presented_ns,
            stats.backend_ready_ns,
            stats.deferred_complete_ns,
            stats.frontend_prepare_us,
            stats.preview_prepare_us,
            stats.module_load_us,
            stats.module_create_us,
            stats.pump_processing_count,
            stats.pump_processing_total_us,
            stats.pump_processing_max_us,
            stats.egl_init_us,
            stats.ganesh_init_us,
            stats.first_submit_build_us,
            stats.first_render_us,
            stats.first_swap_us};
}

struct Response {
    bool observed{}, during_pending{};
    std::uint64_t issued_ns{}, caller_return_ns{}, applied_ns{}, presented_ns{},
        target_submission{}, presented_submission{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Response, observed, during_pending, issued_ns, caller_return_ns,
                                   applied_ns, presented_ns, target_submission,
                                   presented_submission)

struct AppResult {
    unsigned app{};
    bool passed{}, cleanup{}, cancelled{};
    std::string error, gl_renderer;
    std::uint64_t owner_tid{}, request_ns{}, prewarm_us{}, all_content_verified_ns{}, close_us{},
        presentation_count{}, submission_count{}, component_count{}, deferred_installed{},
        deferred_diagnostics{}, startup_pump_count{};
    Milestones milestones;
    Response pending_response, ready_response;
    std::vector<std::uint64_t> pump_processing_us;
    std::vector<MemoryPoint> memory;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AppResult, app, passed, cleanup, cancelled, error, gl_renderer,
                                   owner_tid, request_ns, prewarm_us, all_content_verified_ns,
                                   close_us, presentation_count, submission_count, component_count,
                                   deferred_installed, deferred_diagnostics, startup_pump_count,
                                   milestones, pending_response, ready_response, pump_processing_us,
                                   memory)

struct Recorder {
    std::string renderer, failure;

    void Event(const prism::contracts::LaunchEvent &event)
    {
        if (event.milestone == prism::contracts::LaunchMilestone::FirstPresented) {
            renderer = event.detail;
        } else if (event.milestone == prism::contracts::LaunchMilestone::Failed) {
            failure = event.detail;
        }
    }
};

class Owner {
public:
    Owner(Host &host, Sources &sources, AppResult &result)
        : host_(host), sources_(sources), result_(result)
    {
    }

    void Pump(bool control = false, int timeout = -1)
    {
        std::vector<pollfd> wake{{sources_.Guard(), POLLIN, 0}};
        if (control) {
            wake.push_back({sources_.Control(), POLLIN, 0});
        }
        const bool running = host_.Pump(timeout, wake);
        const auto stats = host_.GetStartupStats();
        if (stats.pump_processing_count != observed_turns_) {
            result_.pump_processing_us.push_back(stats.pump_processing_last_us);
            observed_turns_ = stats.pump_processing_count;
        }
        Require(!wake.front().revents, "Real demo startup/control deadline exceeded");
        Require(running, "Real demo Host stopped during startup/control");
        if (control) {
            Require(wake.back().revents & POLLIN, "Caller readiness lost during startup");
        }
        if (observed_turns_ % 8 == 0) {
            result_.memory.push_back(Memory());
        }
    }

    Response Apply(const Theme &theme, bool pending)
    {
        Response response;
        response.observed = true;
        response.during_pending = pending;
        response.issued_ns = Now();
        sources_.Signal();
        Pump(true);
        response.caller_return_ns = Now();
        sources_.Drain();
        response.target_submission = host_.GetUiState().submission_count + 1;
        std::string detail;
        auto changed = theme;
        changed.generation = host_.ThemeGeneration() + 1;
        Require(host_.ApplyTheme(changed, &detail), "Real demo rejected changed theme snapshot");
        response.applied_ns = Now();
        return response;
    }

    void Feedback(Response &response)
    {
        const auto ui = host_.GetUiState();
        if (response.observed && !response.presented_ns &&
            ui.last_presented_submission >= response.target_submission) {
            response.presented_ns = Now();
            response.presented_submission = ui.last_presented_submission;
        }
    }

private:
    Host &host_;
    Sources &sources_;
    AppResult &result_;
    std::uint64_t observed_turns_{};
};

struct Input {
    std::string socket;
    prism::launch::AppPackage package;
    Theme dark, light;
    std::shared_ptr<prism::runtime::SessionTaskBudget> budget;
    unsigned workers{};
    bool warm{};
};

bool ContentReady(const Startup &stats, const prism::sdk::HostUiState &ui)
{
    return stats.backend_ready_ns && stats.deferred_complete_ns && ui.master_presented &&
           !ui.business_work_pending && !ui.deferred_diagnostics;
}

void AppThread(const Input &input, unsigned app, bool exercise_pending, AppResult &result)
{
    result.app = app;
    result.owner_tid = static_cast<std::uint64_t>(syscall(SYS_gettid));
    try {
        Sources sources;
        Recorder recorder;
        prism::sdk::HostConfig config;
        config.socket = input.socket;
        config.instance = {app + 1};
        config.request = {app + 1};
        config.task_workers = input.workers;
        config.task_budget = input.budget;
        config.initial_theme = input.dark;
        config.on_event = std::bind_front(&Recorder::Event, &recorder);
        Host host(std::move(config));
        Owner owner(host, sources, result);
        if (input.warm) {
            const auto start = Now();
            Require(host.PrepareFrontend(), "Real demo frontend prewarm failed");
            result.prewarm_us = (Now() - start) / 1000;
        }
        result.request_ns = Now();
        Require(host.Bind(input.package), "Real demo Bind failed");
        result.memory.push_back(Memory());

        for (;;) {
            const auto stats = host.GetStartupStats();
            const auto ui = host.GetUiState();
            if (exercise_pending && !result.pending_response.observed && ui.preview_presented &&
                !ContentReady(stats, ui)) {
                result.pending_response = owner.Apply(input.light, true);
            }
            owner.Feedback(result.pending_response);
            if (ContentReady(stats, ui) &&
                (!result.pending_response.observed || result.pending_response.presented_ns)) {
                break;
            }
            owner.Pump();
        }
        Require(recorder.renderer.find("V3D") != std::string::npos,
                "Real demo did not present through V3D");
        Require(recorder.failure.empty(), "Real demo emitted Failed");
        // Verify a current visible frame after all regions/data are installed.
        // Hidden Library is not drawn; this is NOT first all-content presentation.
        result.startup_pump_count = result.pump_processing_us.size();
        const auto &changed = result.pending_response.observed ? input.dark : input.light;
        result.ready_response = owner.Apply(changed, false);
        while (!result.ready_response.presented_ns) {
            owner.Pump();
            owner.Feedback(result.ready_response);
        }
        result.all_content_verified_ns = result.ready_response.presented_ns;
        const auto ui = host.GetUiState();
        result.milestones = Observe(host.GetStartupStats());
        const auto &times = result.milestones;
        Require(times.bind_ns && times.preview_submitted_ns && times.preview_presented_ns &&
                    times.master_queued_ns && times.master_prepared_ns &&
                    times.master_installed_ns && times.master_submitted_ns &&
                    times.master_presented_ns && times.backend_ready_ns &&
                    times.deferred_complete_ns,
                "Real demo lacks a required owner-observed startup milestone");
        Require(times.preview_submitted_ns <= times.master_queued_ns &&
                    times.preview_presented_ns <= times.master_installed_ns &&
                    times.master_prepared_ns <= times.master_installed_ns &&
                    times.master_installed_ns <= times.master_submitted_ns &&
                    times.master_submitted_ns <= times.master_presented_ns,
                "Real demo startup milestones violate pipeline ordering");
        result.gl_renderer = recorder.renderer;
        result.presentation_count = ui.presentation_count;
        result.submission_count = ui.submission_count;
        result.component_count = ui.component_count;
        result.deferred_installed = ui.deferred_installed;
        result.deferred_diagnostics = ui.deferred_diagnostics;
        result.memory.push_back(Memory());
        Require(ui.master_presented && ui.component_count > 1 && ui.deferred_installed > 0,
                "Production Music did not use critical/deferred graph");
        const auto closing = Now();
        host.Close();
        result.close_us = (Now() - closing) / 1000;
        const auto closed = host.GetUiState();
        Require(!closed.business_work_pending && !closed.business_work_completion_ready,
                "Close retained pending business work observations");
        result.cleanup = result.passed = true;
    } catch (const std::exception &error) {
        result.error = error.what();
    }
}

AppResult Cancel(const Input &input)
{
    AppResult result;
    result.app = 999;
    try {
        Sources sources;
        Recorder recorder;
        prism::sdk::HostConfig config;
        config.socket = input.socket;
        config.request = {999};
        config.instance = {999};
        config.task_workers = input.workers;
        config.task_budget = input.budget;
        config.initial_theme = input.dark;
        config.on_event = std::bind_front(&Recorder::Event, &recorder);
        Host host(std::move(config));
        result.request_ns = Now();
        Require(host.Bind(input.package), "Cancellation demo Bind failed");
        Owner owner(host, sources, result);
        while (!host.GetUiState().master_load.owner) {
            owner.Pump();
        }
        const auto before = host.GetUiState();
        const auto start = Now();
        host.Close();
        result.close_us = (Now() - start) / 1000;
        const auto closed = host.GetUiState();
        result.cancelled = closed.cancelled;
        Require(!closed.business_work_pending && !closed.business_work_completion_ready &&
                    result.close_us < 5000000,
                "Cancellation did not promptly close real demo work");
        Require(before.master_presented || closed.cancelled,
                "Queued unfinished load did not become cancelled");
        result.cleanup = result.passed = true;
    } catch (const std::exception &error) {
        result.error = error.what();
    }
    return result;
}

struct ProcessUsage {
    std::uint64_t user_us{}, system_us{}, peak_rss_kib{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ProcessUsage, user_us, system_us, peak_rss_kib)

ProcessUsage Usage()
{
    rusage value{};
    Require(getrusage(RUSAGE_SELF, &value) == 0, "Cannot collect process CPU/RSS");
    return {static_cast<std::uint64_t>(value.ru_utime.tv_sec) * 1000000 + value.ru_utime.tv_usec,
            static_cast<std::uint64_t>(value.ru_stime.tv_sec) * 1000000 + value.ru_stime.tv_usec,
            static_cast<std::uint64_t>(value.ru_maxrss)};
}

struct Iteration {
    unsigned index{};
    bool passed{}, leases_released{};
    std::uint64_t started_ns{}, finished_ns{};
    ProcessUsage process_delta;
    std::vector<AppResult> apps;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Iteration, index, passed, leases_released, started_ns,
                                   finished_ns, process_delta, apps)

struct Report {
    unsigned schema_version{1}, workers{}, app_count{}, iterations{};
    bool warm{}, supplementary{true}, passed{};
    std::string gate{"real-music-startup"}, error;
    std::string scope{
        "Direct AppHost assignment with prevalidated real package and resolved theme; "
        "warm means PrepareFrontend, cold means unprepared Host, neither is OS cold "
        "cache or production launcher pool. Two hosts share one process, distinct "
        "owner threads and active task budget; PSS is process-wide."};
    std::string timing{
        "Owner-observed steady-clock ns; processing samples exclude actual poll wait "
        "but include drawing/dispatch and descheduling, not pure CPU or GPU timing. "
        "PSS sampled outside Pump every "
        "8 turns and at milestones. all_content_verified includes an extra changed "
        "theme frame of the current visible page after all regions are installed; "
        "hidden Library is not drawn. Not first full-content presentation; no pointer latency "
        "claim."};
    std::vector<Iteration> samples;
    AppResult responsiveness, cancellation;
    ProcessUsage process_total;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Report, schema_version, workers, app_count, iterations, warm,
                                   supplementary, passed, gate, error, scope, timing, samples,
                                   responsiveness, cancellation, process_total)

bool Released(const std::shared_ptr<prism::runtime::SessionTaskBudget> &budget)
{
    const auto stats = budget->Stats();
    return !stats.active && !stats.waiting && !stats.leases && !stats.retained_leases &&
           !stats.reserved_bytes && !stats.retained_bytes;
}

void Execute(Input &input, Report &report)
{
    for (unsigned iteration = 0; iteration < report.iterations; ++iteration) {
        const auto before = Usage();
        Iteration sample;
        sample.index = iteration;
        sample.started_ns = Now();
        sample.apps.resize(report.app_count);
        std::vector<std::jthread> owners;
        for (unsigned app = 0; app < report.app_count; ++app) {
            owners.emplace_back(AppThread, std::cref(input), app, false,
                                std::ref(sample.apps[app]));
        }
        owners.clear(); // jthread joins; each Host is destroyed on its own owner.
        sample.finished_ns = Now();
        const auto after = Usage();
        sample.process_delta = {after.user_us - before.user_us, after.system_us - before.system_us,
                                after.peak_rss_kib};
        sample.leases_released = Released(input.budget);
        sample.passed =
            sample.leases_released && std::all_of(sample.apps.begin(), sample.apps.end(),
                                                  [](const AppResult &app) { return app.passed; });
        report.samples.push_back(std::move(sample));
        Require(report.samples.back().passed,
                "Real demo startup sample failed; see per-app result");
    }
    if (report.supplementary) {
        AppThread(input, report.app_count, true, report.responsiveness);
        Require(report.responsiveness.passed && Released(input.budget),
                "Supplementary response/lease cleanup failed");
        report.cancellation = Cancel(input);
        Require(report.cancellation.passed && Released(input.budget),
                "Real demo cancellation/lease cleanup failed");
    }
    report.passed = true;
}
} // namespace

int main(int argc, char **argv)
{
    Report report;
    std::string destination;
    const auto before = Usage();
    try {
        Require(argc == 8 || argc == 9,
                "Usage: demo_startup_probe socket package workers warm app_count "
                "iterations report.json [supplementary-0-or-1]");
        destination = argv[7];
        report.workers = std::stoul(argv[3]);
        const auto warm = std::stoul(argv[4]);
        Require(warm <= 1, "Warm flag must be 0 or 1");
        report.warm = warm != 0;
        report.app_count = std::stoul(argv[5]);
        report.iterations = std::stoul(argv[6]);
        if (argc == 9) {
            const auto supplementary = std::stoul(argv[8]);
            Require(supplementary <= 1, "Supplementary flag must be 0 or 1");
            report.supplementary = supplementary != 0;
        }
        Require((report.workers == 1 || report.workers == 2) &&
                    (report.app_count == 1 || report.app_count == 2) && report.iterations > 0 &&
                    report.iterations <= 5,
                "Invalid native sample configuration");
        Input input;
        input.socket = argv[1];
        input.package = prism::launch::LoadPackage(argv[2]);
        input.dark = prism::theme::LoadTheme(prism::theme::DefaultThemeRoot(), "glass", 1, "dark");
        input.light =
            prism::theme::LoadTheme(prism::theme::DefaultThemeRoot(), "glass", 2, "light");
        input.workers = report.workers;
        input.warm = report.warm;
        prism::runtime::TaskBudgetConfig config;
        config.active_limit = input.workers;
        input.budget = prism::runtime::SessionTaskBudget::Create(config);
        Execute(input, report);
    } catch (const std::exception &error) {
        report.error = error.what();
    }
    const auto after = Usage();
    report.process_total = {after.user_us - before.user_us, after.system_us - before.system_us,
                            after.peak_rss_kib};
    if (!destination.empty()) {
        std::ofstream output(destination);
        if (output) {
            output << nlohmann::json(report).dump(2) << '\n';
        } else {
            report.passed = false;
            report.error = "Cannot write startup evidence";
        }
    }
    if (!report.passed) {
        std::cerr << "demo_startup_probe: " << report.error << '\n';
        return 1;
    }
    std::cout << "Real Music startup samples passed\n";
    return 0;
}
