#include "service_p.hpp"
#include <stdexcept>
#include <utility>
#include <variant>

namespace prism::launcher {
using detail::Now;
using detail::Require;

void Service::Impl::Deliver(Owner owner, LaunchEvent event)
{
    auto *endpoint = Find(owner);
    if (!endpoint || endpoint->stream->Closed()) {
        return;
    }

    event.request = {owner.request};
    auto frame = owner.worker ? launch::EncodeWorker(launch::WorkerReply{event})
                              : launch::EncodeMessage(event);
    endpoint->stream->Queue(frame);
}

bool Service::Impl::Event(Job &job, LaunchMilestone milestone, LaunchError error,
                          std::string detail, int exit_code)
{
    LaunchEvent event{job.request.request, job.instance,     job.state.Pid(), milestone, error,
                      exit_code,           std::move(detail)};
    if (!job.state.Apply(event)) {
        return false;
    }

    job.history.push_back(event);
    Deliver(job.owner, event);
    return true;
}

void Service::Impl::DeliverUpdate(Owner owner, InstanceUpdate update)
{
    auto *endpoint = Find(owner);
    if (!endpoint || endpoint->stream->Closed()) {
        return;
    }

    update.request = {endpoint->subscription};
    endpoint->stream->Queue(owner.worker ? launch::EncodeWorker(update)
                                         : launch::EncodeMessage(update));
}

void Service::Impl::Subscribe(Owner owner, InstanceSubscribe request)
{
    auto *endpoint = Find(owner);
    if (!endpoint) {
        return;
    }
    if (endpoint->subscription || endpoint->requests.contains(request.request.value) ||
        endpoint->theme_ids.contains(request.request.value)) {
        endpoint->stream->Close();
        return;
    }

    endpoint->subscription = request.request.value;
    DeliverUpdate(owner, {{}, {}, 0, InstanceChange::Reset, {}});
    for (const auto &[id, job] : jobs) {
        if (!job->alias && job->role == WindowRole::Toplevel && job->mapped &&
            !job->state.Terminal()) {
            DeliverUpdate(owner, {{},
                                  job->instance,
                                  job->state.Pid(),
                                  InstanceChange::Running,
                                  job->request.app_id});
        }
    }
    DeliverUpdate(owner, {{}, {}, 0, InstanceChange::SnapshotDone, {}});
}

void Service::Impl::WindowChanged(Job &job, bool mapped)
{
    if (job.mapped == mapped) {
        return;
    }
    job.mapped = mapped;
    if (job.alias || job.role != WindowRole::Toplevel) {
        return;
    }

    InstanceUpdate update{{},
                          job.instance,
                          job.state.Pid(),
                          mapped ? InstanceChange::Running : InstanceChange::Stopped,
                          job.request.app_id};
    for (const auto &[id, endpoint] : clients) {
        if (endpoint.subscription) {
            DeliverUpdate({false, id, 0}, update);
        }
    }
    for (const auto &[pid, worker] : workers) {
        if (worker.subscription) {
            DeliverUpdate({true, static_cast<unsigned>(pid), 0}, update);
        }
    }
}

void Service::Impl::Fail(Job &job, LaunchError error, std::string detail)
{
    if (!job.state.Terminal()) {
        Event(job, LaunchMilestone::Failed, error, std::move(detail));
    }
    if (job.alias) {
        return; // An activation request never owns the existing worker.
    }
    if (job.state.Pid()) {
        SendControl(launch::ControlType::Revoke, job);
    }
    if (auto pid = job.state.Pid()) {
        auto worker = workers.find(pid);
        if (worker != workers.end() && worker->second.job == job.request.request.value) {
            StopWorker(worker->second);
        }
    }
    ShellUnavailable(job);
}

void Service::Impl::BootstrapShell()
{
    unsigned role = 0;
    for (const char *app : {"prism_desktop", "prism_topbar", "prism_dock"}) {
        launch::LoadRegisteredPackage(config.apps_root, app);

        auto id = next_job++;
        auto job =
            std::make_unique<Job>(id, LaunchRequest{{id}, app, LaunchMode::NewInstance}, Owner{});
        job->role = static_cast<WindowRole>(++role);
        Event(*job, LaunchMilestone::Accepted);
        jobs.emplace(id, std::move(job));
    }
}

void Service::Impl::Request(Owner owner, LaunchRequest source)
{
    auto *endpoint = Find(owner);
    if (!endpoint) {
        return;
    }

    owner.request = source.request.value;
    if (owner.request == endpoint->subscription || endpoint->theme_ids.contains(owner.request) ||
        (theme_transaction && theme_transaction->request == owner.request &&
         theme_transaction->owner.worker == owner.worker &&
         theme_transaction->owner.endpoint == owner.endpoint)) {
        endpoint->stream->Close();
        return;
    }
    if (shutting_down) {
        Deliver(owner, {source.request,
                        {},
                        0,
                        LaunchMilestone::Failed,
                        LaunchError::SessionEnded,
                        0,
                        "Launcher session is stopping"});
        return;
    }

    if (auto prior = endpoint->requests.find(owner.request); prior != endpoint->requests.end()) {
        auto found = jobs.find(prior->second);
        if (found == jobs.end() || found->second->request.app_id != source.app_id ||
            found->second->request.mode != source.mode) {
            endpoint->stream->Close();
            return;
        }
        for (auto event : found->second->history) {
            Deliver(owner, event);
        }
        return;
    }
    if (endpoint->requests.size() >= 256 || jobs.size() >= 4096) {
        endpoint->stream->Close();
        return;
    }

    unsigned waiting = 0;
    for (const auto &[id, job] : jobs) {
        if (!job->alias && !job->state.Terminal() && !job->state.Pid()) {
            ++waiting;
        }
    }
    if (waiting >= 64) {
        Deliver(owner, {source.request,
                        {},
                        0,
                        LaunchMilestone::Failed,
                        LaunchError::NoWorker,
                        0,
                        "Pending launch queue is full"});
        return;
    }

    std::uint64_t activation_target = 0;
    try {
        if (source.app_id == "prism_desktop" || source.app_id == "prism_topbar" ||
            source.app_id == "prism_dock") {
            throw launch::LaunchFailure(LaunchError::InvalidRequest,
                                        "Shell packages are private session applications");
        }
        launch::LoadRegisteredPackage(config.apps_root, source.app_id);
        if (source.mode == LaunchMode::ActivateOrCreate) {
            for (const auto &[id, job] : jobs) {
                if (!job->alias && job->role == WindowRole::Toplevel &&
                    job->request.app_id == source.app_id && !job->state.Terminal()) {
                    if (!control) {
                        throw launch::LaunchFailure(LaunchError::InvalidRequest,
                                                    "Existing instance activation requires a "
                                                    "trusted WM control channel; use NewInstance");
                    }
                    activation_target = id;
                    break;
                }
            }
        }
    } catch (const launch::LaunchFailure &error) {
        Deliver(owner,
                {source.request, {}, 0, LaunchMilestone::Failed, error.Code(), 0, error.what()});
        return;
    } catch (const std::exception &) {
        Deliver(owner, {source.request,
                        {},
                        0,
                        LaunchMilestone::Failed,
                        LaunchError::InvalidPackage,
                        0,
                        "Cannot resolve application package"});
        return;
    }

    auto id = next_job++;
    Require(id != 0, "Instance ID space exhausted");
    source.request = {id};
    auto job =
        std::make_unique<Job>(activation_target ? jobs.at(activation_target)->instance.value : id,
                              std::move(source), owner);
    job->alias = activation_target;
    endpoint->requests[owner.request] = id;
    Event(*job, LaunchMilestone::Accepted);
    jobs.emplace(id, std::move(job));
}

void Service::Impl::Cancel(Owner owner, LaunchCancel cancel)
{
    auto *endpoint = Find(owner);
    if (!endpoint) {
        return;
    }

    owner.request = cancel.request.value;
    const auto found = endpoint->requests.find(owner.request);
    if (found == endpoint->requests.end()) {
        Deliver(owner, {cancel.request,
                        {},
                        0,
                        LaunchMilestone::Failed,
                        LaunchError::InvalidRequest,
                        0,
                        "Unknown cancellation target"});
        return;
    }

    auto &job = *jobs.at(found->second);
    if (job.state.Activated()) {
        for (auto event : job.history) {
            Deliver(owner, event);
        }
        return;
    }
    if (!job.state.Terminal()) {
        Fail(job, LaunchError::Cancelled, "Cancelled by request owner");
    } else {
        for (auto event : job.history) {
            Deliver(owner, event);
        }
    }
}

void Service::Impl::ReadClient(std::uint64_t id, Endpoint &endpoint)
{
    try {
        for (auto &frame : endpoint.stream->Receive()) {
            const auto message = launch::DecodeMessage(frame);
            if (const auto *request = std::get_if<LaunchRequest>(&message)) {
                Request({false, id, 0}, *request);
            } else if (const auto *cancel = std::get_if<LaunchCancel>(&message)) {
                Cancel({false, id, 0}, *cancel);
            } else if (const auto *subscribe = std::get_if<InstanceSubscribe>(&message)) {
                Subscribe({false, id, 0}, *subscribe);
            } else if (const auto *theme = std::get_if<ThemeRequest>(&message)) {
                SelectTheme({false, id, 0}, *theme);
            } else {
                endpoint.stream->Close();
            }
            if (endpoint.stream->Closed()) {
                break;
            }
        }

        if (endpoint.stream->HasPartialFrame() && !endpoint.stream->HasCompleteFrame()) {
            if (!endpoint.partial_since) {
                endpoint.partial_since = Now();
            }
            if (Now() - endpoint.partial_since > 5000000000ULL) {
                endpoint.stream->Close();
            }
        } else {
            endpoint.partial_since = 0;
        }
    } catch (...) {
        endpoint.stream->Close();
    }
}

} // namespace prism::launcher
