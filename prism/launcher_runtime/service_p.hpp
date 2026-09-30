#pragma once
#include "prism/contracts/theme.hpp"
#include "prism/launch/control_protocol.hpp"
#include "prism/launch/error.hpp"
#include "prism/launch/instance_state.hpp"
#include "prism/launch/package.hpp"
#include "prism/launch/stream.hpp"
#include "prism/launch/worker_protocol.hpp"
#include "prism/launcher/service.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace prism::launcher::detail {
using namespace contracts;
std::uint64_t Now();
void Require(bool valid, const char *detail);

struct Owner {
    bool worker{};
    std::uint64_t endpoint{}, request{};
};

struct Endpoint {
    std::unique_ptr<launch::Stream> stream;
    std::map<std::uint64_t, std::uint64_t> requests;
    std::uint64_t partial_since{}, subscription{};
    std::map<std::uint64_t, contracts::ThemeEvent> theme_requests;
    std::map<std::uint64_t, std::pair<std::string, std::string>> theme_ids;
};

struct Job {
    Job(std::uint64_t id, LaunchRequest source, Owner endpoint)
        : request(std::move(source)), owner(endpoint), state(request.request, {id}), instance{id},
          created(Now())
    {
    }

    std::uint64_t alias{};
    WindowRole role{WindowRole::Toplevel};
    bool mapped{}, registered{}, activation_sent{}, bound_sent{};
    LaunchRequest request;
    Owner owner;
    launch::InstanceState state;
    InstanceId instance;
    std::vector<LaunchEvent> history;
    std::uint64_t created;
};

struct Worker : Endpoint {
    enum class Phase { Preparing, Idle, Assigned, Finishing, Stopping };
    pid_t pid{};
    Phase phase{Phase::Preparing};
    std::uint64_t job{}, created{}, finish_at{}, kill_at{}, closed_at{}, theme_generation{};
    bool frontend_ready{}, layout_revoked{};
    std::uint64_t layout_subscription{}, layout_revision{};
    std::set<std::uint64_t> layout_requests;
};
} // namespace prism::launcher::detail

namespace prism::launcher {
using namespace contracts;
using detail::Endpoint;
using detail::Job;
using detail::Owner;
using detail::Worker;

// Private state shared by the service's endpoint, transaction and worker stages.
struct Service::Impl {
    explicit Impl(ServiceConfig value);
    ServiceConfig config;
    int listener{-1}, lock{-1};
    std::filesystem::path socket_path;
    dev_t socket_device{};
    ino_t socket_inode{};
    std::map<std::uint64_t, Endpoint> clients;
    std::map<pid_t, Worker> workers;
    std::map<std::uint64_t, std::unique_ptr<Job>> jobs;
    std::uint64_t next_endpoint{1}, next_job{1}, retry_at{};
    bool shutting_down{}, control_failed{};
    int signal_fd{-1};
    std::unique_ptr<launch::Stream> control;
    std::uint64_t session{}, opened_at{};
    launch::LayoutSnapshotCache layout_snapshot;
    ThemeSnapshot theme, committed_theme;
    bool theme_ready{};
    std::shared_ptr<runtime::SessionTaskBudget> load_budget;

    struct ThemeTransaction {
        Owner owner;
        std::uint64_t request{}, deadline{};
        ThemeSnapshot previous;
        std::set<pid_t> pending;
        bool wm_pending{}, rollback{};
        std::string detail;
    };

    std::optional<ThemeTransaction> theme_transaction;

    ~Impl();
    void Open();
    Endpoint *Find(Owner owner);
    void Deliver(Owner owner, LaunchEvent event);
    bool Event(Job &job, LaunchMilestone milestone, LaunchError error = LaunchError::None,
               std::string detail = {}, int exit_code = 0);
    void DeliverUpdate(Owner owner, InstanceUpdate update);
    void Subscribe(Owner owner, InstanceSubscribe request);
    void WindowChanged(Job &job, bool mapped);
    void StopWorker(Worker &worker);
    void FinishWorker(Worker &worker);
    ThemeEvent ThemeResult(std::uint64_t request, ThemeStatus status,
                           std::string detail = {}) const;
    void DeliverTheme(Owner owner, const ThemeEvent &event);
    void SendWmTheme();
    void SendWorkerTheme(Worker &worker);
    void PublishThemeToHosts();
    void FinishTheme();
    void RollbackTheme(std::string detail);
    void ThemeAck(Worker &worker, const ThemeApplied &ack);
    void SelectTheme(Owner owner, const ThemeRequest &request);
    void SendControl(launch::ControlType type, const Job &job, std::uint64_t transaction = 0);
    void Fail(Job &job, LaunchError error, std::string detail);
    void BootstrapShell();
    void ReadControl();
    void SubscribeLayout();
    Job *LayoutOwner(const Worker &worker, bool controls) const;
    void SubscribeWorkerLayout(Worker &worker, const LayoutSubscription &request);
    void PublishWorkerLayout(Worker &worker);
    void PublishLayout();
    void RequestLayoutControl(Worker &worker, const LayoutControlRequest &request);
    void ReceiveLayoutControl(const launch::ControlMessage &message);
    void RevokeWorkerLayout(Worker &worker);
    void DisconnectLayout();
    void Request(Owner owner, LaunchRequest source);
    void Cancel(Owner owner, LaunchCancel cancel);
    void Accept();
    void ReadClient(std::uint64_t id, Endpoint &endpoint);
    void ReadWorker(Worker &worker);
    bool Spawn();
    void Reap();
    void Maintain();
    void Flush();
    int WaitTimeout() const;
    void Shutdown();
    int Run(const volatile std::sig_atomic_t &stopping, int wake_fd);
};
} // namespace prism::launcher
