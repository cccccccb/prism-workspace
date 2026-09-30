#include "service_p.hpp"

namespace prism::launcher {
namespace {
LayoutControlResult Rejected(const LayoutControlRequest &request, LayoutControlError error,
                             const std::shared_ptr<const LayoutSnapshot> &snapshot)
{
    LayoutControlResult result;
    result.request = request.request;
    result.gesture = request.gesture;
    result.session = request.session;
    result.sequence = request.sequence;
    result.error = error;
    if (snapshot) {
        result.revision = snapshot->revision;
        result.topology_revision = snapshot->topology_revision;
        result.layout_revision = snapshot->layout_revision;
    }
    return result;
}
} // namespace

Job *Service::Impl::LayoutOwner(const Worker &worker, bool controls) const
{
    if (worker.phase != Worker::Phase::Assigned || !worker.job || worker.stream->Closed()) {
        return nullptr;
    }
    const auto found = jobs.find(worker.job);
    if (found == jobs.end()) {
        return nullptr;
    }
    auto &job = *found->second;
    const bool allowed =
        job.role == WindowRole::TopBar || (!controls && job.role == WindowRole::Dock);
    if (!allowed || job.alias || !job.registered || !job.bound_sent || job.state.Terminal() ||
        job.state.Pid() != static_cast<unsigned>(worker.pid)) {
        return nullptr;
    }
    return &job;
}

void Service::Impl::SubscribeWorkerLayout(Worker &worker, const LayoutSubscription &request)
{
    if (!LayoutOwner(worker, false)) {
        worker.stream->Queue(
            launch::EncodeWorker(LayoutStateEvent{request.request, LayoutStateStatus::Denied, {}}));
        return;
    }
    worker.layout_subscription = request.enabled ? request.request : 0;
    worker.layout_revision = 0;
    PublishWorkerLayout(worker);
}

void Service::Impl::PublishWorkerLayout(Worker &worker)
{
    const auto snapshot = layout_snapshot.Current();
    if (!worker.layout_subscription || !snapshot || snapshot->revision <= worker.layout_revision ||
        !LayoutOwner(worker, false)) {
        return;
    }

    LayoutStateEvent event{worker.layout_subscription, LayoutStateStatus::Current, *snapshot};
    if (worker.stream->Queue(launch::EncodeWorker(event))) {
        worker.layout_revision = snapshot->revision;
    } else {
        RevokeWorkerLayout(worker);
    }
}

void Service::Impl::PublishLayout()
{
    for (auto &[pid, worker] : workers) {
        PublishWorkerLayout(worker);
    }
}

void Service::Impl::RequestLayoutControl(Worker &worker, const LayoutControlRequest &request)
{
    const auto *job = LayoutOwner(worker, true);
    LayoutControlError error = LayoutControlError::None;
    if (!job || !job->mapped) {
        error = LayoutControlError::Unauthorized;
    } else if (!control || control->Closed() || control_failed || !session) {
        error = LayoutControlError::Disconnected;
    } else if (!worker.layout_requests.contains(request.request) &&
               worker.layout_requests.size() >= 32) {
        error = LayoutControlError::Busy;
    }
    if (error != LayoutControlError::None) {
        worker.stream->Queue(
            launch::EncodeWorker(Rejected(request, error, layout_snapshot.Current())));
        return;
    }

    launch::ControlMessage message;
    message.type = launch::ControlType::LayoutControl;
    message.permit.session = session;
    message.permit.request = job->request.request;
    message.permit.instance = job->instance;
    message.permit.pid = job->state.Pid();
    message.permit.role = job->role;
    message.control_request = request;
    if (control->Queue(launch::EncodeControl(message))) {
        worker.layout_requests.insert(request.request);
    } else {
        control_failed = true;
        worker.stream->Queue(launch::EncodeWorker(
            Rejected(request, LayoutControlError::Disconnected, layout_snapshot.Current())));
    }
}

void Service::Impl::ReceiveLayoutControl(const launch::ControlMessage &message)
{
    const auto &permit = message.permit;
    const auto found = workers.find(permit.pid);
    if (found == workers.end()) {
        return; // A reply can race the owning worker's exit.
    }
    auto &worker = found->second;
    const auto *job = LayoutOwner(worker, true);
    if (!job || job->instance != permit.instance || job->role != permit.role ||
        job->request.request != permit.request) {
        return; // A reused PID never receives a previous instance's response.
    }

    worker.layout_requests.erase(message.control_result.request);
    if (!worker.stream->Queue(launch::EncodeWorker(message.control_result))) {
        RevokeWorkerLayout(worker);
    }
}

void Service::Impl::RevokeWorkerLayout(Worker &worker)
{
    worker.layout_subscription = 0;
    worker.layout_revision = 0;
    worker.layout_requests.clear();
    if (!worker.layout_revoked && worker.job) {
        const auto found = jobs.find(worker.job);
        if (found != jobs.end() && found->second->state.Pid()) {
            SendControl(launch::ControlType::Revoke, *found->second);
        }
    }
    worker.layout_revoked = true;
}

void Service::Impl::DisconnectLayout()
{
    layout_snapshot.Reset();
    for (auto &[pid, worker] : workers) {
        if (worker.layout_subscription && !worker.stream->Closed()) {
            worker.stream->Queue(launch::EncodeWorker(
                LayoutStateEvent{worker.layout_subscription, LayoutStateStatus::Disconnected, {}}));
        }
        worker.layout_subscription = 0;
        worker.layout_revision = 0;
        worker.layout_requests.clear();
    }
}
} // namespace prism::launcher
