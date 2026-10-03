#include "prism/host/event_wait.hpp"
#include "service_p.hpp"
#include <algorithm>
#include <cerrno>
#include <poll.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace prism::launcher {
using detail::Now;
using detail::Require;

void Service::Impl::Maintain()
{
    const auto now = Now();
    if (theme_transaction && now >= theme_transaction->deadline) {
        RollbackTheme("Theme installation timed out");
    }

    for (auto &[id, job] : jobs) {
        if (!job->state.Terminal() && !job->state.Activated() &&
            ((job->role == WindowRole::LayoutControls ? !job->mapped
                                                      : !job->state.FirstPresented()) ||
             !job->state.BackendReady()) &&
            now - job->created >= config.startup_timeout_ms * 1000000ULL) {
            Fail(*job, LaunchError::Timeout,
                 "Worker allocation/presentation/backend watchdog expired");
        }
    }

    for (auto &[pid, worker] : workers) {
        if (worker.phase == Worker::Phase::Finishing && now >= worker.finish_at) {
            StopWorker(worker);
        }
        if (worker.phase == Worker::Phase::Preparing &&
            now - worker.created >= config.startup_timeout_ms * 1000000ULL) {
            StopWorker(worker);
        }
        if (worker.closed_at && now - worker.closed_at >= 250000000ULL) {
            if (worker.job && !jobs.at(worker.job)->state.Terminal()) {
                Fail(*jobs.at(worker.job), LaunchError::RuntimeFailed,
                     "Worker control connection lost");
            }
            StopWorker(worker);
        }
        if (worker.phase == Worker::Phase::Stopping && worker.kill_at && now >= worker.kill_at) {
            kill(pid, SIGKILL);
            worker.kill_at = 0; // Consumed; SIGCHLD wakes reaping.
        }
    }

    if (!theme_transaction && theme_ready) {

        for (auto &[id, job] : jobs) {
            if (job->alias || !job->registered || job->bound_sent || job->state.Terminal()) {
                continue;
            }
            auto found = workers.find(job->state.Pid());
            if (found == workers.end()) {
                continue;
            }
            auto &worker = found->second;
            if (worker.theme_generation != theme.generation ||
                worker.phase != Worker::Phase::Assigned) {
                continue;
            }
            job->bound_sent = true;
            if (!worker.stream->Queue(launch::EncodeWorker(launch::WorkerBind{
                    job->request, job->instance, job->role == WindowRole::LayoutControls}))) {
                Fail(*job, LaunchError::RuntimeFailed, "Worker bind failed");
            }
        }
    }

    for (auto &[id, job] : jobs) {
        if (job->alias && !job->state.Terminal() && !job->state.Activated()) {
            auto target = jobs.find(job->alias);
            if (target == jobs.end() || target->second->state.Terminal()) {
                Fail(*job, LaunchError::RuntimeFailed, "Activation target ended");
                continue;
            }
            auto &original = *target->second;
            if (!job->state.Pid() && original.state.Pid()) {
                LaunchEvent assigned{job->request.request,
                                     job->instance,
                                     original.state.Pid(),
                                     LaunchMilestone::WorkerAssigned,
                                     LaunchError::None,
                                     0,
                                     {}};
                Require(job->state.Apply(assigned), "Activation assignment error");
                job->history.push_back(assigned);
                Deliver(job->owner, assigned);
            }
            if (original.mapped && !job->activation_sent) {
                job->activation_sent = true;
                SendControl(launch::ControlType::Activate, *job);
            }
        }
    }

    for (auto &[id, job] : jobs) {
        if (!job->alias && !job->state.Terminal() && !job->state.Pid() && theme_ready &&
            !theme_transaction && (!control || session)) {
            auto idle = std::find_if(workers.begin(), workers.end(), [&](const auto &item) {
                return item.second.phase == Worker::Phase::Idle &&
                       item.second.theme_generation == theme.generation &&
                       !item.second.stream->Closed();
            });
            if (idle == workers.end()) {
                break;
            }
            auto &worker = idle->second;
            worker.phase = Worker::Phase::Assigned;
            worker.job = id;
            LaunchEvent assigned{job->request.request,
                                 job->instance,
                                 static_cast<unsigned>(worker.pid),
                                 LaunchMilestone::WorkerAssigned,
                                 LaunchError::None,
                                 0,
                                 {}};
            Require(job->state.Apply(assigned), "Internal worker assignment state error");
            job->history.push_back(assigned);
            Deliver(job->owner, assigned);
            if (control) {
                SendControl(launch::ControlType::Grant, *job);
            } else {
                job->bound_sent = true;
                if (!worker.stream->Queue(launch::EncodeWorker(launch::WorkerBind{
                        job->request, job->instance, job->role == WindowRole::LayoutControls}))) {
                    Fail(*job, LaunchError::RuntimeFailed, "Worker assignment send failed");
                }
            }
        }
    }

    unsigned waiting = 0, unbound = 0;

    for (const auto &[id, job] : jobs) {
        if (!job->alias && !job->state.Terminal() && !job->state.Pid()) {
            ++waiting;
        }
    }
    for (const auto &[pid, worker] : workers) {
        if (worker.phase == Worker::Phase::Idle || worker.phase == Worker::Phase::Preparing) {
            ++unbound;
        }
    }

    const auto wanted = std::max(config.pool_size, waiting);
    for (auto it = workers.rbegin(); it != workers.rend() && unbound > wanted; ++it) {
        auto &worker = it->second;
        if (worker.phase == Worker::Phase::Idle || worker.phase == Worker::Phase::Preparing) {
            StopWorker(worker);
            --unbound;
        }
    }
    if (!shutting_down && now >= retry_at && unbound < wanted &&
        workers.size() < config.max_workers) {
        if (!Spawn()) {
            retry_at = now + 1000000000ULL;
        }
    }

    for (auto it = clients.begin(); it != clients.end();) {
        if (it->second.stream->Closed()) {
            it = clients.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = jobs.begin(); it != jobs.end();) {
        // Keep private Shell identities until session shutdown. WM
        // registration/unmap replies may arrive after the worker is reaped;
        // their original identity must remain available for validation.
        if (it->second->role == WindowRole::Toplevel && it->second->state.Terminal() &&
            (!it->second->state.Pid() || it->second->state.ExitCode()) &&
            !Find(it->second->owner)) {
            it = jobs.erase(it);
        } else {
            ++it;
        }
    }
}

void Service::Impl::Flush()
{
    if (control) {
        control->Flush();
    }
    for (auto &[id, endpoint] : clients) {
        endpoint.stream->Flush();
    }

    for (auto &[pid, worker] : workers) {
        worker.stream->Flush();
    }
}

int Service::Impl::WaitTimeout() const
{
    const auto now = Now();
    std::optional<std::uint64_t> deadline;
    if (control && !theme_ready) {
        host::Earlier(deadline, host::After(opened_at, 10000000000ULL));
    }
    if (theme_transaction) {
        host::Earlier(deadline, theme_transaction->deadline);
    }

    for (const auto &[id, endpoint] : clients) {
        if (endpoint.stream->HasCompleteFrame()) {
            return 0;
        }
        if (endpoint.partial_since) {
            host::Earlier(deadline, host::After(endpoint.partial_since, 5000000000ULL));
        }
    }
    if (control && control->HasCompleteFrame()) {
        return 0;
    }

    for (const auto &[id, job] : jobs) {
        if (!job->state.Terminal() && !job->state.Activated() &&
            ((job->role == WindowRole::LayoutControls ? !job->mapped
                                                      : !job->state.FirstPresented()) ||
             !job->state.BackendReady())) {
            host::Earlier(deadline,
                          host::After(job->created, config.startup_timeout_ms * 1000000ULL));
        }
    }

    unsigned waiting{}, unbound{};

    for (const auto &[id, job] : jobs) {
        if (!job->alias && !job->state.Terminal() && !job->state.Pid()) {
            ++waiting;
        }
    }
    for (const auto &[pid, worker] : workers) {
        if (worker.stream->HasCompleteFrame()) {
            return 0;
        }
        if (worker.phase == Worker::Phase::Idle || worker.phase == Worker::Phase::Preparing) {
            ++unbound;
        }
        if (worker.phase == Worker::Phase::Preparing) {
            host::Earlier(deadline,
                          host::After(worker.created, config.startup_timeout_ms * 1000000ULL));
        }
        if (worker.phase == Worker::Phase::Finishing) {
            host::Earlier(deadline, worker.finish_at);
        }
        if (worker.closed_at && worker.phase != Worker::Phase::Stopping) {
            host::Earlier(deadline, host::After(worker.closed_at, 250000000ULL));
        }
        if (worker.phase == Worker::Phase::Stopping && worker.kill_at) {
            host::Earlier(deadline, worker.kill_at);
        }
    }
    if (unbound < std::max(config.pool_size, waiting) && workers.size() < config.max_workers) {
        host::Earlier(deadline, retry_at);
    }

    // Only legacy direct callers without a signal source need a reaping
    // fallback. The production executable always supplies its self-pipe.
    return host::Timeout(now, deadline, signal_fd < 0 ? 1000 : -1);
}

void Service::Impl::Shutdown()
{
    if (shutting_down) {
        return;
    }
    shutting_down = true;

    for (auto &[id, job] : jobs) {
        Fail(*job, LaunchError::SessionEnded, "Launcher session ended");
    }

    for (auto &[pid, worker] : workers) {
        StopWorker(worker);
    }

    const auto deadline = Now() + 2000000000ULL;
    while (!workers.empty() && Now() < deadline) {

        for (auto &[pid, worker] : workers) {
            ReadWorker(worker);
            if (worker.kill_at && Now() >= worker.kill_at) {
                kill(pid, SIGKILL);
                worker.kill_at = 0;
            }
        }
        Reap();
        Flush();
        if (workers.empty()) {
            break;
        }
        std::optional<std::uint64_t> next = deadline;
        std::vector<pollfd> wait;
        if (signal_fd >= 0) {
            wait.push_back({signal_fd, POLLIN, 0});
        }
        for (const auto &[pid, worker] : workers) {
            if (worker.kill_at) {
                host::Earlier(next, worker.kill_at);
            }
            if (!worker.stream->Closed()) {
                wait.push_back(
                    {worker.stream->Fd(),
                     static_cast<short>(POLLIN | (worker.stream->WantsWrite() ? POLLOUT : 0)), 0});
            }
        }
        const auto result =
            poll(wait.data(), wait.size(), host::Timeout(Now(), next, signal_fd < 0 ? 1000 : -1));
        if (result < 0 && errno != EINTR) {
            break;
        }
        if (signal_fd >= 0 && !wait.empty() && wait.front().revents) {
            host::Drain(signal_fd);
        }
    }

    for (auto &[pid, worker] : workers) {
        kill(pid, SIGKILL);
        while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
        }
        if (load_budget) {
            load_budget->DropProcess(pid);
        }
    }
    workers.clear();
    Flush();
}

int Service::Impl::Run(const volatile std::sig_atomic_t &stopping, int wake_fd)
{
    signal_fd = wake_fd;
    Open();
    while (!stopping && !control_failed) {
        ReadControl();
        if (control && !theme_ready && Now() - opened_at >= 10000000000ULL) {
            control_failed = true;
        }
        Accept();
        for (auto &[id, endpoint] : clients) {
            ReadClient(id, endpoint);
        }

        for (auto &[pid, worker] : workers) {
            ReadWorker(worker);
        }
        Reap();
        Maintain();
        Flush();
        if (stopping || control_failed) {
            break;
        }

        std::vector<pollfd> descriptors{{listener, POLLIN, 0}};
        if (signal_fd >= 0) {
            descriptors.push_back({signal_fd, POLLIN, 0});
        }
        if (control && !control->Closed()) {
            descriptors.push_back(
                {control->Fd(), static_cast<short>(POLLIN | (control->WantsWrite() ? POLLOUT : 0)),
                 0});
        }

        for (const auto &[id, endpoint] : clients) {
            if (!endpoint.stream->Closed()) {
                descriptors.push_back(
                    {endpoint.stream->Fd(),
                     static_cast<short>(POLLIN | (endpoint.stream->WantsWrite() ? POLLOUT : 0)),
                     0});
            }
        }
        for (const auto &[pid, worker] : workers) {
            if (!worker.stream->Closed()) {
                descriptors.push_back(
                    {worker.stream->Fd(),
                     static_cast<short>(POLLIN | (worker.stream->WantsWrite() ? POLLOUT : 0)), 0});
            }
        }

        const int result = poll(descriptors.data(), descriptors.size(), WaitTimeout());
        if (result < 0 && errno != EINTR) {
            throw std::runtime_error("Launcher event wait failed");
        }
        if (signal_fd >= 0 && descriptors[1].revents) {
            host::Drain(signal_fd);
        }
    }
    Shutdown();
    return control_failed ? 1 : 0;
}

} // namespace prism::launcher
