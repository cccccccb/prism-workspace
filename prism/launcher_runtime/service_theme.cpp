#include "prism/theme/compiler.hpp"
#include "service_p.hpp"
#include <iostream>
#include <stdexcept>
#include <utility>

namespace prism::launcher {
using detail::Now;
using detail::Require;

ThemeEvent Service::Impl::ThemeResult(std::uint64_t request, ThemeStatus status,
                                      std::string detail) const
{
    return {request,
            committed_theme.generation,
            status,
            committed_theme.id,
            committed_theme.name,
            std::move(detail),
            committed_theme.color_scheme};
}

void Service::Impl::DeliverTheme(Owner owner, const ThemeEvent &event)
{
    if (auto *endpoint = Find(owner)) {
        if (event.request) {
            endpoint->theme_requests[event.request] = event;
        }
        endpoint->stream->Queue(owner.worker ? launch::EncodeWorker(event)
                                             : launch::EncodeMessage(event));
    }
}

void Service::Impl::SendWmTheme()
{
    if (!control || !session) {
        return;
    }
    launch::ControlMessage m;
    m.type = launch::ControlType::InstallTheme;
    m.permit.session = session;
    m.theme = theme;
    if (!control->Queue(launch::EncodeControl(m))) {
        control_failed = true;
    }
}

void Service::Impl::SendWorkerTheme(Worker &worker)
{
    if (worker.phase == Worker::Phase::Stopping || worker.phase == Worker::Phase::Finishing ||
        !worker.frontend_ready || worker.stream->Closed()) {
        return;
    }
    if (theme_transaction) {
        theme_transaction->pending.insert(worker.pid);
    }
    if (!worker.stream->Queue(launch::EncodeWorker(theme))) {
        StopWorker(worker);
    }
}

void Service::Impl::PublishThemeToHosts()
{
    auto event = ThemeResult(0, ThemeStatus::Current);
    for (auto &[pid, worker] : workers) {
        if (worker.phase == Worker::Phase::Assigned && !worker.stream->Closed()) {
            worker.stream->Queue(launch::EncodeWorker(event));
        }
    }
}

void Service::Impl::FinishTheme()
{
    if (!theme_transaction || theme_transaction->wm_pending ||
        !theme_transaction->pending.empty()) {
        return;
    }

    auto t = std::move(*theme_transaction);
    theme_transaction.reset();
    committed_theme = theme;

    DeliverTheme(t.owner,
                 ThemeResult(t.request, t.rollback ? ThemeStatus::Rejected : ThemeStatus::Applied,
                             t.detail));
    PublishThemeToHosts();
    std::cout << "theme applied id=" << theme.id << " scheme=" << theme.color_scheme
              << " generation=" << theme.generation << " rollback=" << t.rollback << std::endl;
}

void Service::Impl::RollbackTheme(std::string detail)
{
    if (!theme_transaction) {
        control_failed = true;
        return;
    }

    auto &t = *theme_transaction;
    if (t.rollback) {
        std::cerr << "Theme rollback failed: " << detail << std::endl;
        control_failed = true;
        return;
    }

    t.rollback = true;
    t.detail = std::move(detail);
    t.deadline = Now() + 5000000000ULL;
    t.pending.clear();
    t.wm_pending = control != nullptr;
    const auto next = theme.generation + 1;
    Require(next, "Theme generation exhausted");
    theme = t.previous;
    theme.generation = next;

    SendWmTheme();
    for (auto &[pid, worker] : workers) {
        SendWorkerTheme(worker);
    }
    FinishTheme();
}

void Service::Impl::ThemeAck(Worker &worker, const ThemeApplied &ack)
{
    Require(worker.frontend_ready && ack.generation <= theme.generation,
            "Unexpected worker theme ACK");
    if (ack.generation != theme.generation) {
        return; // An older in-flight candidate can finish during rollback.
    }
    if (!ack.success) {
        if (theme_transaction) {
            RollbackTheme("Host rejected theme: " + ack.detail);
        } else {
            throw std::runtime_error("Host rejected initial theme: " + ack.detail);
        }
        return;
    }

    worker.theme_generation = ack.generation;
    if (worker.phase == Worker::Phase::Preparing) {
        worker.phase = Worker::Phase::Idle;
    }
    if (theme_transaction) {
        theme_transaction->pending.erase(worker.pid);
        FinishTheme();
    }
}

void Service::Impl::SelectTheme(Owner owner, const ThemeRequest &request)
{
    auto *endpoint = Find(owner);
    if (!endpoint) {
        return;
    }

    Require(!endpoint->requests.contains(request.request) &&
                endpoint->subscription != request.request,
            "Theme request ID reused");
    if (auto identity = endpoint->theme_ids.find(request.request);
        identity != endpoint->theme_ids.end()) {
        Require(identity->second == std::pair{request.id, request.color_scheme},
                "Theme replay ID refers to another appearance request");
        if (auto prior = endpoint->theme_requests.find(request.request);
            prior != endpoint->theme_requests.end()) {
            DeliverTheme(owner, prior->second);
        }
        return;
    }

    if (endpoint->theme_requests.size() >= 256) {
        endpoint->theme_ids.erase(endpoint->theme_requests.begin()->first);
        endpoint->theme_requests.erase(endpoint->theme_requests.begin());
    }
    endpoint->theme_ids.emplace(request.request, std::pair{request.id, request.color_scheme});
    if (request.id.empty() && request.color_scheme.empty()) {
        DeliverTheme(owner, ThemeResult(request.request, ThemeStatus::Current));
        return;
    }
    if (shutting_down || theme_transaction || !theme_ready) {
        DeliverTheme(owner,
                     ThemeResult(request.request, ThemeStatus::Rejected, "Theme service is busy"));
        return;
    }

    ThemeSnapshot candidate;
    try {
        candidate = prism::theme::LoadTheme(
            config.themes_root, request.id.empty() ? committed_theme.id : request.id,
            theme.generation + 1,
            request.color_scheme.empty() ? committed_theme.color_scheme : request.color_scheme);
    } catch (const std::exception &e) {
        DeliverTheme(owner, ThemeResult(request.request, ThemeStatus::Rejected, e.what()));
        return;
    }

    if (candidate.layout != committed_theme.layout) {
        DeliverTheme(
            owner,
            ThemeResult(
                request.request, ThemeStatus::Rejected,
                "Live Shell/BSP geometry changes require a configure-aware theme transaction"));
        return;
    }
    if (candidate.id == committed_theme.id &&
        candidate.color_scheme == committed_theme.color_scheme &&
        candidate.numbers == committed_theme.numbers &&
        candidate.colors == committed_theme.colors &&
        candidate.materials == committed_theme.materials &&
        candidate.layout == committed_theme.layout && candidate.normal == committed_theme.normal &&
        candidate.focused == committed_theme.focused &&
        candidate.fullscreen == committed_theme.fullscreen &&
        candidate.controls == committed_theme.controls) {
        DeliverTheme(owner, ThemeResult(request.request, ThemeStatus::Applied));
        return;
    }

    ThemeTransaction t;
    t.owner = owner;
    t.request = request.request;
    t.previous = committed_theme;
    t.deadline = Now() + 5000000000ULL;
    t.wm_pending = control != nullptr;
    theme_transaction = std::move(t);
    theme = std::move(candidate);

    SendWmTheme();
    for (auto &[pid, worker] : workers) {
        SendWorkerTheme(worker);
    }
    FinishTheme();
}

} // namespace prism::launcher
