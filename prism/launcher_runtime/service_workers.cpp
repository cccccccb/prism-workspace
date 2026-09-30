#include "prism/host/event_wait.hpp"
#include "service_p.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <spawn.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <variant>

extern char **environ;

namespace prism::launcher {
using detail::Now;
using detail::Require;

namespace {
class WorkerDescriptors {
public:
    explicit WorkerDescriptors(int budget)
    {
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair)) {
            return;
        }
        parent_ = pair[0];
        control_ = fcntl(pair[1], F_DUPFD_CLOEXEC, 10);
        close(pair[1]);
        budget_ = fcntl(budget, F_DUPFD_CLOEXEC, 10);
    }

    ~WorkerDescriptors()
    {
        if (parent_ >= 0) {
            close(parent_);
        }
        if (control_ >= 0) {
            close(control_);
        }
        if (budget_ >= 0) {
            close(budget_);
        }
    }

    bool Valid() const
    {
        return parent_ >= 0 && control_ >= 0 && budget_ >= 0;
    }

    int Configure(posix_spawn_file_actions_t &actions) const
    {
        int result = posix_spawn_file_actions_adddup2(&actions, control_, 3);
        if (!result) {
            result = posix_spawn_file_actions_adddup2(&actions, budget_, 4);
        }
        if (!result) {
            result = posix_spawn_file_actions_addclose(&actions, control_);
        }
        if (!result) {
            result = posix_spawn_file_actions_addclose(&actions, budget_);
        }
        return result;
    }

    int TakeParent()
    {
        return std::exchange(parent_, -1);
    }

private:
    int parent_{-1}, control_{-1}, budget_{-1};
};
} // namespace

void Service::Impl::StopWorker(Worker &worker)
{
    if (worker.phase != Worker::Phase::Stopping) {
        RevokeWorkerLayout(worker);
        worker.phase = Worker::Phase::Stopping;
        worker.finish_at = 0;
        worker.kill_at = Now() + 1000000000ULL;
        kill(worker.pid, SIGTERM);
    }
}

void Service::Impl::FinishWorker(Worker &worker)
{
    // A validated terminal report means the host is already exiting. Give
    // normal cleanup/exit a bounded turn before sending enforcement signals.
    // Cancellation, watchdog and session shutdown still call StopWorker.
    if (worker.phase != Worker::Phase::Stopping && worker.phase != Worker::Phase::Finishing) {
        RevokeWorkerLayout(worker);
        worker.phase = Worker::Phase::Finishing;
        worker.finish_at = host::After(Now(), 250000000ULL);
    }
}

void Service::Impl::ReadWorker(Worker &worker)
{
    try {
        for (auto &frame : worker.stream->Receive()) {
            const auto message = launch::DecodeWorker(frame);
            if (const auto *ready = std::get_if<launch::WorkerReady>(&message)) {
                Require(worker.phase == Worker::Phase::Preparing, "Unexpected worker Ready");
                worker.frontend_ready = true;
                SendWorkerTheme(worker);
                std::cout << "worker ready pid=" << worker.pid
                          << " preparation_ns=" << ready->preparation_ns << std::endl;
            } else if (const auto *ack = std::get_if<ThemeApplied>(&message)) {
                ThemeAck(worker, *ack);
            } else if (const auto *request = std::get_if<ThemeRequest>(&message)) {
                Require(worker.phase == Worker::Phase::Assigned, "Theme request from idle worker");
                SelectTheme({true, static_cast<unsigned>(worker.pid), 0}, *request);
            } else if (const auto *subscription = std::get_if<LayoutSubscription>(&message)) {
                SubscribeWorkerLayout(worker, *subscription);
            } else if (const auto *request = std::get_if<LayoutControlRequest>(&message)) {
                RequestLayoutControl(worker, *request);
            } else if (const auto *event = std::get_if<LaunchEvent>(&message)) {
                Require(worker.job != 0, "Event from idle worker");
                Require(!control ||
                            (jobs.at(worker.job)->registered && jobs.at(worker.job)->bound_sent),
                        "Worker event before bind/WM registration ACK");
                auto &job = *jobs.at(worker.job);
                Require(event->pid == static_cast<unsigned>(worker.pid) &&
                            event->request == job.request.request &&
                            event->instance == job.instance &&
                            event->milestone != LaunchMilestone::Accepted &&
                            event->milestone != LaunchMilestone::WorkerAssigned &&
                            event->milestone != LaunchMilestone::Exited &&
                            event->milestone != LaunchMilestone::Activated,
                        "Worker launch identity/milestone mismatch");
                if (!job.state.Terminal()) {
                    Require(job.state.Apply(*event), "Out-of-order worker event");
                    job.history.push_back(*event);
                    Deliver(job.owner, *event);
                    std::cout << "instance=" << job.instance.value << " app=" << job.request.app_id
                              << " pid=" << event->pid
                              << " milestone=" << static_cast<unsigned>(event->milestone)
                              << " error=" << static_cast<unsigned>(event->error)
                              << " detail=" << event->detail << std::endl;
                    if (!control && event->milestone == LaunchMilestone::FirstPresented) {
                        WindowChanged(job, true);
                    }
                    if (event->milestone == LaunchMilestone::Failed) {
                        SendControl(launch::ControlType::Revoke, job);
                        FinishWorker(worker); // Blocked cleanup still reaches TERM then KILL.
                        if (job.role != WindowRole::Toplevel) {
                            control_failed = true;
                        }
                    }
                }
            } else if (const auto *request = std::get_if<LaunchRequest>(&message)) {
                Require(worker.phase == Worker::Phase::Assigned && worker.job != 0 &&
                            !jobs.at(worker.job)->state.Terminal(),
                        "Launch from inactive worker");
                Request({true, static_cast<unsigned>(worker.pid), 0}, *request);
            } else if (const auto *subscribe = std::get_if<InstanceSubscribe>(&message)) {
                Require(worker.phase == Worker::Phase::Assigned, "Subscribe from idle worker");
                Subscribe({true, static_cast<unsigned>(worker.pid), 0}, *subscribe);
            } else if (const auto *cancel = std::get_if<LaunchCancel>(&message)) {
                Require(worker.phase == Worker::Phase::Assigned, "Cancel from idle worker");
                Cancel({true, static_cast<unsigned>(worker.pid), 0}, *cancel);
            } else {
                throw std::runtime_error("Unexpected worker message");
            }
        }
    } catch (...) {
        worker.stream->Close();
        if (worker.job) {
            Fail(*jobs.at(worker.job), LaunchError::RuntimeFailed,
                 "Worker control protocol failed");
        }
        StopWorker(worker);
    }

    if (worker.stream->Closed() && !worker.closed_at) {
        RevokeWorkerLayout(worker);
        worker.closed_at = Now();
    }
}

bool Service::Impl::Spawn()
{
    WorkerDescriptors descriptors(load_budget->Fd());
    if (!descriptors.Valid()) {
        return false;
    }

    posix_spawn_file_actions_t actions;
    int result = posix_spawn_file_actions_init(&actions);
    if (result) {
        return false;
    }
    result = descriptors.Configure(actions);
    // Both sources were duplicated above FD4 before any child dup2. All other
    // service descriptors are CLOEXEC, including the parent socket endpoint.
    const auto path = config.host.string(), root = config.apps_root.string(),
               parent = std::to_string(getpid());
    const char *words[]{path.c_str(),       "--worker-fd", "3",
                        "--apps-root",      root.c_str(),  "--parent-pid",
                        parent.c_str(),     "--wayland",   config.wayland.c_str(),
                        "--load-budget-fd", "4",           nullptr};
    pid_t pid = 0;
    if (!result) {
        result = posix_spawn(&pid, path.c_str(), &actions, nullptr,
                             const_cast<char *const *>(words), environ);
    }
    posix_spawn_file_actions_destroy(&actions);
    if (result) {
        std::cerr << "worker spawn failed: " << std::strerror(result) << '\n';
        return false;
    }

    Worker worker;
    worker.pid = pid;
    worker.created = Now();
    worker.stream =
        std::make_unique<launch::Stream>(descriptors.TakeParent(), launch::WorkerFrameSize);
    workers.emplace(pid, std::move(worker));
    std::cout << "worker spawned pid=" << pid << std::endl;
    return true;
}

void Service::Impl::Reap()
{
    for (auto it = workers.begin(); it != workers.end();) {
        auto &worker = it->second;
        int status = 0;
        const auto reaped = waitpid(worker.pid, &status, WNOHANG);
        if (reaped < 0 && errno == ECHILD) {
            // This PID may already have been reused; never signal it now.
            if (worker.job) {
                Event(*jobs.at(worker.job), LaunchMilestone::Failed, LaunchError::RuntimeFailed,
                      "Worker was reaped outside the service");
            }
            workers.erase(it);
            throw std::runtime_error("Worker reaping ownership was lost");
        }
        if (reaped <= 0) {
            ++it;
            continue;
        }

        load_budget->DropProcess(worker.pid);

        if (worker.job) {
            auto &job = *jobs.at(worker.job);
            const int exit_code = WIFSIGNALED(status) ? -WTERMSIG(status) : WEXITSTATUS(status);
            if (!job.state.Terminal() &&
                (exit_code != 0 || !job.state.FirstPresented() || !job.state.BackendReady())) {
                Event(job, LaunchMilestone::Failed, LaunchError::RuntimeFailed,
                      "Worker exited before successful completion or crashed");
            }
            SendControl(launch::ControlType::Revoke, job);
            WindowChanged(job, false);
            Event(job, LaunchMilestone::Exited, LaunchError::None, {}, exit_code);
            for (auto &[id, alias] : jobs) {
                if (alias->alias == worker.job) {
                    if (!alias->state.Pid()) {
                        Event(*alias, LaunchMilestone::Failed, LaunchError::RuntimeFailed,
                              "Activation target exited before assignment");
                    } else {
                        if (!alias->state.Terminal() && !alias->state.Activated()) {
                            Event(*alias, LaunchMilestone::Failed, LaunchError::RuntimeFailed,
                                  "Activation target exited");
                        }
                        Event(*alias, LaunchMilestone::Exited, LaunchError::None, {}, exit_code);
                    }
                }
            }
            if (job.role != WindowRole::Toplevel && !shutting_down) {
                control_failed = true;
            }
        } else if (worker.phase == Worker::Phase::Preparing) {
            retry_at = Now() + 1000000000ULL;
        }

        std::cout << "worker reaped pid=" << worker.pid << std::endl;
        if (theme_transaction) {
            theme_transaction->pending.erase(worker.pid);
        }
        it = workers.erase(it);
        FinishTheme();
    }
}

} // namespace prism::launcher
