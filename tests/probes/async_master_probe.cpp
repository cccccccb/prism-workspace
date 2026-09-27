#include "prism/runtime/load_session.hpp"
#include "prism/sdk/app_host.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using Host = prism::sdk::AppHost;
using UiState = prism::sdk::HostUiState;
using prism::contracts::LaunchEvent;
using prism::contracts::LaunchMilestone;
using prism::runtime::ComponentSource;
using prism::runtime::LoadFailure;
using prism::runtime::LoadStage;
using prism::runtime::PreparedComponent;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

void Require(bool condition, std::string_view detail)
{
    if (!condition) {
        throw std::runtime_error(std::string(detail));
    }
}

class WakeFd {
public:
    WakeFd() : fd_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC))
    {
        Require(fd_ >= 0, "Cannot create probe eventfd");
    }

    ~WakeFd()
    {
        close(fd_);
    }

    WakeFd(const WakeFd &) = delete;
    WakeFd &operator=(const WakeFd &) = delete;

    int Fd() const
    {
        return fd_;
    }

    void Signal()
    {
        const std::uint64_t value = 1;
        ssize_t size;
        do {
            size = write(fd_, &value, sizeof(value));
        } while (size < 0 && errno == EINTR);
        Require(size == sizeof(value), "Probe eventfd signal failed");
    }

    void Drain()
    {
        std::uint64_t value;
        ssize_t size;
        do {
            size = read(fd_, &value, sizeof(value));
        } while (size < 0 && errno == EINTR);
        Require(size == sizeof(value), "Host consumed or lost caller-owned eventfd readiness");
    }

private:
    int fd_;
};

// The same real compiler runs after the gate. Only this test adapter supplies a
// deterministic scheduling barrier; no product file format or startup mode changes.
class PrepareBarrier {
public:
    PreparedComponent Prepare(std::string_view text, ComponentSource source, std::stop_token stop)
    {
        {
            std::unique_lock lock(mutex_);
            worker_ = std::this_thread::get_id();
            ++calls_;
            entered_ = true;
            wake_.Signal();
            if (!released_.wait(lock, stop, std::bind_front(&PrepareBarrier::IsReleased, this))) {
                cancelled_ = true;
                throw LoadFailure({LoadStage::Cancelled, source, 0, "Probe preparation cancelled"});
            }
        }
        auto prepared = prism::runtime::PrepareComponent(text, std::move(source));
        if (stop.stop_requested()) {
            throw LoadFailure({LoadStage::Cancelled, prepared.Source(), 0,
                               "Probe preparation cancelled after compile"});
        }
        return prepared;
    }

    bool Entered() const
    {
        std::lock_guard lock(mutex_);
        return entered_;
    }

    void Release()
    {
        {
            std::lock_guard lock(mutex_);
            proceed_ = true;
        }
        released_.notify_all();
    }

    bool Cancelled() const
    {
        std::lock_guard lock(mutex_);
        return cancelled_;
    }

    void CheckWorker() const
    {
        std::lock_guard lock(mutex_);
        Require(calls_ == 1 && worker_ != std::this_thread::get_id(),
                "Master compiler did not execute once on a background worker");
    }

    WakeFd &Wake()
    {
        return wake_;
    }

private:
    bool IsReleased() const
    {
        return proceed_;
    }

    mutable std::mutex mutex_;
    std::condition_variable_any released_;
    WakeFd wake_;
    std::thread::id worker_;
    unsigned calls_{};
    bool entered_{}, proceed_{}, cancelled_{};
};

struct RecordedEvent {
    LaunchEvent event;
    UiState ui;
};

struct Recorder {
    Host *host{};
    std::thread::id owner{std::this_thread::get_id()};
    std::vector<RecordedEvent> events;

    void Record(const LaunchEvent &event)
    {
        Require(host != nullptr, "Probe event recorder has no owner");
        Require(owner == std::this_thread::get_id(),
                "Host emitted launch event on compiler thread");
        events.push_back({event, host->GetUiState()});
    }

    unsigned Count(LaunchMilestone milestone) const
    {
        return static_cast<unsigned>(
            std::count_if(events.begin(), events.end(), [milestone](const RecordedEvent &item) {
                return item.event.milestone == milestone;
            }));
    }

    const RecordedEvent &Find(LaunchMilestone milestone) const
    {
        const auto found =
            std::find_if(events.begin(), events.end(), [milestone](const RecordedEvent &item) {
                return item.event.milestone == milestone;
            });
        Require(found != events.end(), "Required host milestone was not observed");
        return *found;
    }
};

prism::contracts::ThemeSnapshot Theme(std::uint64_t generation)
{
    prism::contracts::ThemeSnapshot theme;
    theme.id = "async-probe";
    theme.name = "Async Master probe";
    theme.generation = generation;
    theme.colors = {{"foreground", generation == 1 ? prism::contracts::Color{235, 240, 250, 255}
                                                   : prism::contracts::Color{160, 210, 250, 255}}};
    return theme;
}

std::uint64_t RegisterLaunch(std::string_view app_id)
{
    Require(app_id == "async-probe-other", "Fixture requested an unexpected application");
    return 9;
}

std::uint64_t RegisterInstances()
{
    return 9;
}

prism::sdk::HostConfig Config(const std::string &socket, Recorder &events,
                              const std::shared_ptr<PrepareBarrier> &barrier = {})
{
    prism::sdk::HostConfig config;
    config.socket = socket;
    config.initial_theme = Theme(1);
    config.on_event = std::bind_front(&Recorder::Record, &events);
    config.launch_app = RegisterLaunch;
    config.subscribe_instances = RegisterInstances;
    if (barrier) {
        config.prepare_component = std::bind_front(&PrepareBarrier::Prepare, barrier);
    }
    return config;
}

void CheckPresentation(const UiState &ui)
{
    if (ui.master_presented) {
        Require(ui.master_installed && ui.master_submitted && ui.master_first_submission != 0 &&
                    ui.master_presented_submission >= ui.master_first_submission,
                "MasterPresented lacks a real submission identity of its installed UI load");
    } else {
        Require(ui.master_presented_submission == 0,
                "Master presentation identity was recorded before actual feedback");
    }
}

void WaitWake(Host &host, WakeFd &wake)
{
    const auto deadline = Clock::now() + 5s;
    for (;;) {
        pollfd fd{wake.Fd(), POLLIN, 0};
        Require(host.Pump(-1, std::span(&fd, 1)), "Host stopped while awaiting external wake");
        Require(Clock::now() < deadline, "External wake was not returned by host wait");
        Require(!(fd.revents & (POLLERR | POLLHUP | POLLNVAL)), "External wake fd failed");
        if (fd.revents & POLLIN) {
            wake.Drain();
            return;
        }
    }
}

void WaitEntered(Host &host, PrepareBarrier &barrier)
{
    WaitWake(host, barrier.Wake());
    Require(barrier.Entered(), "Compiler entry notification preceded actual worker entry");
    barrier.CheckWorker();
    const auto ui = host.GetUiState();
    Require(ui.master_load.owner != 0 && !ui.master_prepared && !ui.master_installed &&
                !ui.master_submitted && !ui.master_presented,
            "Master changed frontend state while its pure compiler was blocked");
}

void WaitPreview(Host &host)
{
    const auto deadline = Clock::now() + 5s;
    while (!host.GetUiState().preview_presented) {
        Require(Clock::now() < deadline,
                "Preview actual presentation did not arrive during prepare");
        Require(host.Pump(20), "Host stopped before Preview actual presentation");
    }
    Require(!host.GetUiState().master_presented,
            "Preview feedback was incorrectly attributed to pending Master");
}

void PendingControls(Host &host)
{
    WakeFd external;
    external.Signal();
    WaitWake(host, external);

    std::string diagnostic;
    Require(host.ApplyTheme(Theme(2), &diagnostic) && diagnostic.empty() &&
                host.ThemeGeneration() == 2,
            "Theme installation did not respond while Master compiler was pending");
    // Business does not exist yet. These owner entrypoints must remain safe;
    // their event projections are checked only after registered business starts.
    host.DeliverThemeEvent({0,
                            2,
                            prism::contracts::ThemeStatus::Applied,
                            "async-probe",
                            "Async Master probe",
                            {},
                            "dark"});
    host.DeliverLaunchEvent({{9}, {10}, 1, LaunchMilestone::Activated});
    host.DeliverInstanceEvent(
        {{9}, {10}, 1, prism::contracts::InstanceChange::Running, "async-probe-other"});
    Require(!host.GetUiState().master_prepared && !host.GetUiState().master_installed,
            "Owner control handling unexpectedly released or installed blocked Master");
}

void WaitMaster(Host &host, Recorder &events, bool preview)
{
    const auto deadline = Clock::now() + 8s;
    for (;;) {
        const auto ui = host.GetUiState();
        CheckPresentation(ui);
        if (ui.master_presented && events.Count(LaunchMilestone::BackendReady)) {
            break;
        }
        Require(Clock::now() < deadline, "Master actual presentation/business readiness timed out");
        Require(host.Pump(20), "Host stopped before Master actual presentation");
    }
    const auto ui = host.GetUiState();
    Require(ui.master_prepared && ui.master_installed && !ui.master_diagnostic && !ui.failed,
            "Successful Master left preparation/install in a failed state");
    Require(events.Count(LaunchMilestone::FirstPresented) == 1 &&
                events.Count(LaunchMilestone::BackendReady) == 1 &&
                events.Count(LaunchMilestone::Failed) == 0,
            "Host first-presentation/business milestones were duplicated or failed");
    const auto &first = events.Find(LaunchMilestone::FirstPresented);
    Require(preview ? first.ui.preview_presented && !first.ui.master_presented
                    : first.ui.master_presented && first.ui.preview_load.owner == 0,
            "FirstPresented does not identify the first actually presented UI");
    Require(first.event.detail.find("V3D") != std::string::npos,
            "Native async Master gate did not use V3D");
    const auto &ready = events.Find(LaunchMilestone::BackendReady);
    Require(ready.ui.master_installed && (!preview || ready.ui.preview_presented),
            "Business became ready before permitted Master installation");
    std::cout << "master_load=" << ui.master_load.owner << ':' << ui.master_load.generation
              << " first_submission=" << ui.master_first_submission
              << " presented_submission=" << ui.master_presented_submission
              << " read_us=" << ui.read_us << " prepare_us=" << ui.prepare_us << '\n';
}

void WaitBusinessPresentation(Host &host, std::uint64_t prior_submission)
{
    const auto deadline = Clock::now() + 5s;
    while (host.GetUiState().master_presented_submission <= prior_submission) {
        Require(Clock::now() < deadline,
                "Business control binding did not receive actual feedback");
        Require(host.Pump(20), "Host stopped while handling business control message");
        CheckPresentation(host.GetUiState());
    }
}

void CheckBusinessControls(Host &host, Recorder &events)
{
    auto prior = host.GetUiState().master_presented_submission;
    host.DeliverLaunchEvent({{9}, {10}, 1, LaunchMilestone::Activated});
    WaitBusinessPresentation(host, prior);

    prior = host.GetUiState().master_presented_submission;
    host.DeliverInstanceEvent(
        {{9}, {10}, 1, prism::contracts::InstanceChange::Running, "async-probe-other"});
    WaitBusinessPresentation(host, prior);

    prior = host.GetUiState().master_presented_submission;
    host.DeliverThemeEvent({0,
                            host.ThemeGeneration(),
                            prism::contracts::ThemeStatus::Applied,
                            "async-probe",
                            "Async Master probe",
                            {},
                            "dark"});
    WaitBusinessPresentation(host, prior);
    Require(events.Count(LaunchMilestone::FirstPresented) == 1 &&
                events.Count(LaunchMilestone::BackendReady) == 1,
            "Business control feedback repeated launch first-presentation/readiness milestones");
}

void CheckSuccess(const std::string &socket, const std::filesystem::path &path, bool preview,
                  bool controlled)
{
    auto barrier = controlled ? std::make_shared<PrepareBarrier>() : nullptr;
    Recorder events;
    Host host(Config(socket, events, barrier));
    events.host = &host;
    Require(host.Bind(prism::launch::LoadPackage(path)), "Host Bind rejected valid async package");
    if (barrier) {
        WaitEntered(host, *barrier);
        if (preview) {
            WaitPreview(host);
            Require(events.Count(LaunchMilestone::FirstPresented) == 1,
                    "Preview FirstPresented missing while Master was blocked");
        } else {
            Require(events.events.empty() && host.GetUiState().preview_load.owner == 0,
                    "Master-only Bind opened/presented synchronously while compiler was blocked");
        }
        PendingControls(host);
        Require(events.Count(LaunchMilestone::BackendReady) == 0,
                "Business started while pure Master preparation was pending");
        barrier->Release();
    }
    WaitMaster(host, events, preview);
    if (controlled) {
        CheckBusinessControls(host, events);
    }
    host.Close();
    std::cout << "scenario=" << path.filename().string() << " passed\n";
}

void CheckFailure(const std::string &socket, const std::filesystem::path &path, LoadStage stage)
{
    auto barrier = std::make_shared<PrepareBarrier>();
    Recorder events;
    Host host(Config(socket, events, barrier));
    events.host = &host;
    const auto package = prism::launch::LoadPackage(path);
    if (stage == LoadStage::Read) {
        Require(std::filesystem::remove(package.ui), "Could not arrange source read failure");
    }
    Require(host.Bind(package), "Preview Bind failed before asynchronous Master failure");
    if (stage != LoadStage::Read) {
        WaitEntered(host, *barrier);
        WaitPreview(host);
        PendingControls(host);
        barrier->Release();
    }

    const auto deadline = Clock::now() + 5s;
    while (host.Pump(20)) {
        Require(Clock::now() < deadline, "Invalid Master did not terminate startup");
    }
    const auto ui = host.GetUiState();
    Require(ui.failed && ui.preview_presented && !ui.master_installed && !ui.master_submitted &&
                !ui.master_presented && ui.master_diagnostic &&
                ui.master_diagnostic->stage == stage &&
                ui.master_diagnostic->source.component_id == "master" &&
                ui.master_diagnostic->source.source_path == package.ui.string() &&
                !ui.master_diagnostic->message.empty(),
            "Master failure lost typed source diagnostic or replaced the presented Preview");
    Require(events.Count(LaunchMilestone::Failed) == 1 &&
                events.Count(LaunchMilestone::FirstPresented) == 1 &&
                events.Count(LaunchMilestone::BackendReady) == 0,
            "Failed preparation reported incorrect/duplicate public milestones");
    const auto error = events.Find(LaunchMilestone::Failed).event.error;
    Require(error == (stage == LoadStage::Read ? prism::contracts::LaunchError::InvalidPackage
                                               : prism::contracts::LaunchError::RuntimeFailed),
            "Typed preparation failure mapped to the wrong public launch error");
    Require(stage == LoadStage::Read || ui.master_diagnostic->line > 0,
            "Parser/schema diagnostic omitted its DSL location");
    Require(host.ApplyTheme(Theme(3)) && host.ThemeGeneration() == 3,
            "Preview Scene was destroyed before terminal startup Close");
    host.Close();
    std::cout << "scenario=" << path.filename().string()
              << " diagnostic_stage=" << static_cast<unsigned>(stage) << " passed\n";
}

void CheckControlPriority(const std::string &socket, const std::filesystem::path &path)
{
    auto barrier = std::make_shared<PrepareBarrier>();
    Recorder events;
    Host host(Config(socket, events, barrier));
    events.host = &host;
    Require(host.Bind(prism::launch::LoadPackage(path)), "Control-priority package Bind failed");
    WaitEntered(host, *barrier);
    WaitPreview(host);

    // Keep this caller-owned source readable across completion publication. A
    // compiler-return signal alone would not prove the LoadSession result is ready.
    WakeFd control;
    control.Signal();
    barrier->Release();
    const auto deadline = Clock::now() + 5s;
    do {
        pollfd fd{control.Fd(), POLLIN, 0};
        Require(host.Pump(20, std::span(&fd, 1)),
                "Host failed before caller handled ready control");
        Require(fd.revents & POLLIN, "Host omitted outstanding caller control readiness");
        const auto ui = host.GetUiState();
        Require(!ui.master_installed && !ui.master_submitted && !ui.master_presented,
                "Host installed/created Master before ready caller control could be handled");
        Require(Clock::now() < deadline,
                "Master completion was not consumed with caller control ready");
        std::this_thread::yield();
    } while (!host.GetUiState().master_prepared);
    Require(
        events.Count(LaunchMilestone::BackendReady) == 0,
        "Host created business before caller handled simultaneous completion/control readiness");

    // Exercise the already-held-candidate branch too. The current theme cannot
    // resolve @after_control until the outer owner handles the queued change.
    pollfd fd{control.Fd(), POLLIN, 0};
    Require(host.Pump(-1, std::span(&fd, 1)) && (fd.revents & POLLIN) &&
                !host.GetUiState().master_installed,
            "Held Master candidate bypassed newly ready caller control");
    auto latest = Theme(3);
    latest.colors.push_back({"after_control", {130, 210, 240, 255}});
    Require(host.ApplyTheme(latest) && host.ThemeGeneration() == 3,
            "Owner could not apply latest theme before held Master installation");
    control.Drain();

    WaitMaster(host, events, true);
    Require(host.ThemeGeneration() == 3 && !host.GetUiState().master_diagnostic,
            "Master installation used stale theme rather than handled control result");
    host.Close();
    std::cout << "scenario=control-completion-priority passed\n";
}

void CheckCancellation(const std::string &socket, const std::filesystem::path &path, bool preview)
{
    auto barrier = std::make_shared<PrepareBarrier>();
    Recorder events;
    Host host(Config(socket, events, barrier));
    events.host = &host;
    Require(host.Bind(prism::launch::LoadPackage(path)), "Cancellation package Bind failed");
    WaitEntered(host, *barrier);
    if (preview) {
        WaitPreview(host);
    }
    PendingControls(host);
    host.Close();
    Require(barrier->Cancelled(), "Close failed to cancel the blocked pure preparation worker");
    Require(host.GetUiState().cancelled && !host.GetUiState().master_installed &&
                !host.GetUiState().master_presented && !host.Pump(0),
            "Closed Host accepted or presented the cancelled completion");
    Require(events.Count(LaunchMilestone::BackendReady) == 0 &&
                events.Count(LaunchMilestone::Failed) == 0,
            "Explicit Close invented business readiness or a startup failure");
    host.Close();
    std::cout << "scenario=cancel-" << (preview ? "preview" : "master-only") << " passed\n";
}

class GraphBarrier {
public:
    PreparedComponent Prepare(std::string_view text, ComponentSource source, std::stop_token stop)
    {
        const bool deferred = source.component_id == "late";
        {
            std::unique_lock lock(mutex_);
            Require(source.component_id == "first" || source.component_id == "second" ||
                        source.component_id == "third" || deferred,
                    "Graph compiler adapter received an unexpected component");
            Require(!workers_.contains(source.component_id), "Graph component prepared twice");
            workers_.emplace(source.component_id, std::this_thread::get_id());
            wake_.Signal();
            if (!released_.wait(lock, stop,
                                std::bind_front(&GraphBarrier::MayProceed, this, deferred))) {
                throw LoadFailure(
                    {LoadStage::Cancelled, source, 0, "Graph probe preparation cancelled"});
            }
        }
        return prism::runtime::PrepareComponent(text, std::move(source));
    }

    bool Entered(std::string_view component) const
    {
        std::lock_guard lock(mutex_);
        return workers_.contains(std::string(component));
    }

    void CheckParallel() const
    {
        std::lock_guard lock(mutex_);
        Require(workers_.contains("first") && workers_.contains("second") &&
                    workers_.at("first") != workers_.at("second") &&
                    workers_.at("first") != std::this_thread::get_id() &&
                    workers_.at("second") != std::this_thread::get_id(),
                "Independent critical graph units did not overlap on distinct CPU workers");
    }

    void Release(bool deferred = false)
    {
        {
            std::lock_guard lock(mutex_);
            if (deferred) {
                deferred_released_ = true;
            } else {
                critical_released_ = true;
            }
        }
        released_.notify_all();
    }

    WakeFd &Wake()
    {
        return wake_;
    }

private:
    bool MayProceed(bool deferred) const
    {
        return deferred ? deferred_released_ : critical_released_;
    }

    mutable std::mutex mutex_;
    std::condition_variable_any released_;
    std::map<std::string, std::thread::id> workers_;
    WakeFd wake_;
    bool critical_released_{}, deferred_released_{};
};

void CheckGraphPresentationGate(Host &host, GraphBarrier &barrier)
{
    const auto ui = host.GetUiState();
    CheckPresentation(ui);
    if (!ui.master_presented) {
        Require(!ui.deferred_started && ui.deferred_prepared == 0 && !barrier.Entered("late"),
                "Deferred graph task started before actual Master presentation");
    }
    if (ui.master_submitted) {
        Require(ui.preview_presented && ui.critical_prepared == 3 && ui.master_images_ready &&
                    ui.master_image_count == 1,
                "Master pixels submitted before Preview/critical units/required image readiness");
    }
}

void WaitParallelGraph(Host &host, GraphBarrier &barrier)
{
    while (!barrier.Entered("first") || !barrier.Entered("second")) {
        WaitWake(host, barrier.Wake());
        CheckGraphPresentationGate(host, barrier);
    }
    barrier.CheckParallel();
    const auto ui = host.GetUiState();
    Require(ui.component_count == 4 && ui.critical_prepared == 0 && !ui.master_installed &&
                !ui.master_submitted && !barrier.Entered("third"),
            "Blocked critical graph changed installation or bypassed after dependency");
}

void PumpGraphControl(Host &host, WakeFd &control)
{
    pollfd fd{control.Fd(), POLLIN, 0};
    Require(host.Pump(20, std::span(&fd, 1)) && (fd.revents & POLLIN),
            "Graph Host lost caller-owned ready control");
    Require(!host.GetUiState().master_installed && !host.GetUiState().master_submitted,
            "Graph Master installed before caller consumed its control source");
}

void SetGraphTheme(Host &host)
{
    auto latest = Theme(3);
    latest.colors.push_back({"after_control", {130, 210, 240, 255}});
    Require(host.ApplyTheme(latest) && host.ThemeGeneration() == 3,
            "Graph owner could not install the theme selected by its ready control");
}

void CheckGraphSuccess(Host &host, Recorder &events, GraphBarrier &barrier, WakeFd &control)
{
    const auto deadline = Clock::now() + 8s;
    while (!host.GetUiState().master_prepared) {
        Require(Clock::now() < deadline, "Critical graph composition did not complete");
        PumpGraphControl(host, control);
        CheckGraphPresentationGate(host, barrier);
    }
    const auto candidate = host.GetUiState();
    Require(candidate.critical_prepared == 3 && !candidate.master_installed &&
                !candidate.deferred_started && events.Count(LaunchMilestone::BackendReady) == 0,
            "Held graph candidate bypassed critical, caller-control or business ordering");
    SetGraphTheme(host);
    control.Drain();

    while (!host.GetUiState().master_presented || !events.Count(LaunchMilestone::BackendReady)) {
        Require(Clock::now() < deadline, "Graph Master pixels/presentation timed out");
        Require(host.Pump(20), "Graph Host stopped before successful Master presentation");
        CheckGraphPresentationGate(host, barrier);
    }
    WaitMaster(host, events, true);
    const auto &ready = events.Find(LaunchMilestone::BackendReady);
    Require(ready.ui.master_images_ready && ready.ui.master_image_count == 1 &&
                ready.ui.critical_prepared == 3,
            "Business started before required critical image decode completion");

    while (!barrier.Entered("late")) {
        WaitWake(host, barrier.Wake());
        CheckGraphPresentationGate(host, barrier);
    }
    auto ui = host.GetUiState();
    Require(ui.master_presented && ui.deferred_started && ui.deferred_prepared == 0,
            "Deferred entry did not follow actual Master presentation");
    const auto first_submission = ui.master_first_submission;
    barrier.Release(true);
    while (host.GetUiState().deferred_prepared != 1) {
        Require(Clock::now() < deadline, "Deferred graph CPU preparation did not complete");
        Require(host.Pump(20), "Deferred graph preparation failed the running Master");
    }
    ui = host.GetUiState();
    Require(!ui.failed && ui.deferred_diagnostics == 0 && ui.master_installed &&
                ui.master_first_submission == first_submission,
            "Deferred CPU completion replaced or failed the critical Master identity");
    CheckBusinessControls(host, events);
    std::cout << "graph_components=" << ui.component_count
              << " critical_prepared=" << ui.critical_prepared
              << " deferred_prepared=" << ui.deferred_prepared
              << " required_images=" << ui.master_image_count
              << " deferred_mount=not_implemented passed\n";
}

void CheckGraphFailure(Host &host, Recorder &events, WakeFd &control,
                       const std::filesystem::path &path, bool image_failure)
{
    PumpGraphControl(host, control);
    SetGraphTheme(host);
    control.Drain();
    const auto deadline = Clock::now() + 8s;
    while (host.Pump(20)) {
        Require(Clock::now() < deadline, "Invalid graph did not finish with a typed failure");
    }
    const auto ui = host.GetUiState();
    Require(ui.failed && ui.preview_presented && !ui.master_installed && !ui.master_submitted &&
                !ui.master_presented && !ui.deferred_started && ui.master_diagnostic &&
                events.Count(LaunchMilestone::FirstPresented) == 1 &&
                events.Count(LaunchMilestone::Failed) == 1 &&
                events.Count(LaunchMilestone::BackendReady) == 0,
            "Graph failure replaced Preview, started business or lost typed diagnostics");
    const auto &diagnostic = *ui.master_diagnostic;
    Require(diagnostic.stage == (image_failure ? LoadStage::ResourceLink : LoadStage::Read) &&
                !diagnostic.message.empty(),
            "Graph failure lost its resource/read stage");
    if (!image_failure) {
        Require(diagnostic.source.component_id == "third" &&
                    diagnostic.source.source_path == (path / "components/third.prism").string(),
                "Component read failure lost its source identity");
    } else {
        Require(ui.master_prepared && ui.master_image_count == 1 && !ui.master_images_ready,
                "Invalid required image did not gate graph pixel publication");
    }
    Require(host.ApplyTheme(Theme(4)), "Failed graph destroyed its preserved Preview frontend");
    std::cout << "graph_diagnostic_stage=" << static_cast<unsigned>(diagnostic.stage)
              << " component=" << diagnostic.source.component_id << " passed\n";
}

void CheckGraph(const std::string &socket, const std::filesystem::path &path)
{
    auto barrier = std::make_shared<GraphBarrier>();
    Recorder events;
    auto config = Config(socket, events);
    config.prepare_component = std::bind_front(&GraphBarrier::Prepare, barrier);
    Host host(std::move(config));
    events.host = &host;
    Require(host.Bind(prism::launch::LoadPackage(path)), "Valid v2 graph Bind failed");
    WaitParallelGraph(host, *barrier);
    WaitPreview(host);
    Require(events.Count(LaunchMilestone::FirstPresented) == 1 &&
                events.Count(LaunchMilestone::BackendReady) == 0,
            "Blocked graph did not preserve presented Preview without business");
    const bool missing_source = path.filename() == "graph-bad-source";
    const bool broken_image = path.filename() == "graph-bad-image";
    if (missing_source) {
        Require(std::filesystem::remove(path / "components/third.prism"),
                "Could not remove dependent component after graph read preflight");
    }
    WakeFd control;
    control.Signal();
    PumpGraphControl(host, control);
    barrier->Release();
    if (missing_source || broken_image) {
        CheckGraphFailure(host, events, control, path, broken_image);
    } else {
        CheckGraphSuccess(host, events, *barrier, control);
    }
    host.Close();
    std::cout << "scenario=" << path.filename().string() << " passed\n";
}

void VerifyGraphs(const std::string &socket, const std::filesystem::path &packages)
{
    CheckGraph(socket, packages / "graph-critical");
    CheckGraph(socket, packages / "graph-bad-source");
    CheckGraph(socket, packages / "graph-bad-image");
}

void Verify(const std::string &socket, const std::filesystem::path &packages)
{
    CheckSuccess(socket, packages / "preview", true, true);
    CheckSuccess(socket, packages / "master-only", false, true);
    CheckSuccess(socket, packages / "default-compiler", true, false);
    CheckControlPriority(socket, packages / "control-priority");
    CheckFailure(socket, packages / "bad-semantic", LoadStage::Semantic);
    CheckFailure(socket, packages / "bad-syntax", LoadStage::Syntax);
    CheckFailure(socket, packages / "bad-read", LoadStage::Read);
    CheckCancellation(socket, packages / "cancel-preview", true);
    CheckCancellation(socket, packages / "cancel-master-only", false);
}
} // namespace

int main(int argc, char **argv)
{
    try {
        Require(argc == 3 || argc == 5,
                "Usage: async_master_probe <wayland-socket> <test-packages-root> "
                "[--mode all|legacy|graph]");
        const std::string_view mode = argc == 3 ? "legacy" : argv[4];
        Require(argc == 3 || std::string_view(argv[3]) == "--mode", "Unknown probe option");
        Require(mode == "all" || mode == "legacy" || mode == "graph", "Unknown probe mode");
        if (mode != "graph") {
            Verify(argv[1], argv[2]);
        }
        if (mode != "legacy") {
            VerifyGraphs(argv[1], argv[2]);
        }
        std::cout << "Async Master owner-thread integration gates passed.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "async_master_probe: " << error.what() << '\n';
        return 1;
    }
}
