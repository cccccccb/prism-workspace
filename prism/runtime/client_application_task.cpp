#include "client_application_p.hpp"

#include <stdexcept>
#include <utility>

namespace prism::sdk {
void ClientApplication::Impl::RevokeOwnerTaskScope()
{
    const auto binding = std::exchange(owner_task_scope, {});
    if (binding && scene && binding->ui == installed_ui) {
        scene->EndOwnerModal(binding->token);
        ClearOwnerConfirmation();
        ClearOwnerFilePanel();
        CollectGestureEvents();
        CollectControlEvents();
    }
}

void ClientApplication::Impl::CancelCurrentOwnerTask(runtime::TaskCancelReason reason)
{
    if (owner_tasks) {
        if (const auto active = owner_tasks->Active()) {
            owner_tasks->Cancel(active->identity, reason);
        }
    }
    RevokeOwnerTaskScope();
}

void ClientApplication::Impl::ReconcileOwnerTask()
{
    if (!scene) {
        CancelCurrentOwnerTask(runtime::TaskCancelReason::ScopeUnavailable);
        return;
    }

    for (const auto &closure : scene->TakeOwnerModalClosures()) {
        if (!owner_task_scope || owner_task_scope->ui != installed_ui ||
            owner_task_scope->token != closure.token) {
            continue;
        }
        const auto reason = closure.reason == runtime::OwnerModalCloseReason::Escape
                                ? runtime::TaskCancelReason::Escape
                                : runtime::TaskCancelReason::ScopeUnavailable;
        CancelCurrentOwnerTask(reason);
    }
    if (owner_task_scope && (owner_task_scope->ui != installed_ui ||
                             owner_task_scope->token != scene->OwnerModalToken())) {
        CancelCurrentOwnerTask(runtime::TaskCancelReason::ScopeUnavailable);
    }
}

void ClientApplication::Impl::AdoptOwnerTaskInput(
    const std::shared_ptr<const runtime::InputSnapshot> &input, runtime::UiLoadId ui)
{
    ReconcileOwnerTask();
    if (!owner_tasks || !owner_task_scope || !input || !scene || ui != installed_ui ||
        owner_task_scope->ui != ui || owner_task_scope->token != scene->OwnerModalToken() ||
        !scene->IsInputSnapshotAdopted(*input)) {
        return;
    }
    const auto active = owner_tasks->Active();
    if (active && active->phase == runtime::TaskPhase::Preparing) {
        owner_tasks->SetReady(owner_task_scope->identity);
    }
}

void ClientApplication::Impl::RetireOwnerTasks(runtime::TaskCancelReason reason)
{
    owner_tasks_retired = true;
    CancelCurrentOwnerTask(reason);
    ClearOwnerConfirmation();
    ClearOwnerFilePanel();
    if (owner_tasks) {
        owner_tasks->RetireOwner();
        owner_tasks->TakeTerminal();
    }
    if (scene) {
        scene->TakeOwnerModalClosures();
    }
}

void ClientApplication::Impl::PublishOwnerTaskChange()
{
    ResetPopupSurface();
    InvalidateQueuedFrame();
    PublishFramePacket();
    QueueRenderUpdate(true);
    if (bridge->terminal.Reason() != runtime::TerminalReason::None) {
        throw std::runtime_error("Owner task render update unavailable");
    }
    CollectGestureEvents();
    CollectControlEvents();
    DeliverInteractionEvents();
}

std::optional<runtime::TaskIdentity> ClientApplication::BeginOwnerTask(std::string_view region,
                                                                       std::uint64_t seat)
{
    auto &app = *impl_;
    if (app.closed || app.failed || app.owner_tasks_retired || !app.opened_once || !app.scene ||
        app.bridge->terminal.Reason() != runtime::TerminalReason::None || !app.installed_ui.owner ||
        region.empty() || !app.scene->RegionMounted(region)) {
        return std::nullopt;
    }

    app.ReconcileOwnerTask();
    const auto root = app.scene->RegionId(region);
    if (!root || !app.scene->IsVisible(root) || !app.scene->State(root).enabled) {
        return std::nullopt;
    }
    if (!app.owner_tasks) {
        app.owner_tasks = std::make_unique<runtime::TaskSession>(runtime::IssueTaskOwnerId());
    }
    const auto identity = app.owner_tasks->Begin();
    if (!identity) {
        return std::nullopt;
    }

    std::optional<std::uint64_t> token;
    try {
        token = app.scene->BeginOwnerModal(root, seat);
    } catch (...) {
        app.owner_tasks->Cancel(*identity, runtime::TaskCancelReason::ScopeUnavailable);
        app.owner_tasks->TakeTerminal();
        throw;
    }
    if (!token) {
        app.owner_tasks->Cancel(*identity, runtime::TaskCancelReason::ScopeUnavailable);
        app.owner_tasks->TakeTerminal();
        return std::nullopt;
    }

    app.owner_task_scope = Impl::OwnerTaskScope{*identity, app.installed_ui, *token};
    try {
        app.PublishOwnerTaskChange();
    } catch (...) {
        // A cancelled input callback may synchronously finish this request
        // and begin another. Roll back only the request started by this call.
        if (app.owner_tasks->Cancel(*identity, runtime::TaskCancelReason::ScopeUnavailable)) {
            if (app.owner_task_scope && app.owner_task_scope->identity == *identity) {
                app.RevokeOwnerTaskScope();
            }
            app.owner_tasks->TakeTerminal();
        }
        throw;
    }
    return identity;
}

std::optional<runtime::TaskEntry> ClientApplication::ActiveOwnerTask() const noexcept
{
    return impl_->owner_tasks && !impl_->owner_tasks_retired ? impl_->owner_tasks->Active()
                                                             : std::nullopt;
}

bool ClientApplication::SetOwnerTaskWorking(runtime::TaskIdentity identity)
{
    impl_->ReconcileOwnerTask();
    return !impl_->closed && !impl_->failed && impl_->owner_tasks &&
           impl_->owner_tasks->SetWorking(identity);
}

bool ClientApplication::ResumeOwnerTask(runtime::TaskIdentity identity)
{
    impl_->ReconcileOwnerTask();
    const auto active = ActiveOwnerTask();
    return !impl_->closed && !impl_->failed && active && active->identity == identity &&
           active->phase == runtime::TaskPhase::Working && impl_->owner_tasks->SetReady(identity);
}

bool ClientApplication::CompleteOwnerTask(runtime::TaskIdentity identity)
{
    impl_->ReconcileOwnerTask();
    if (impl_->closed || impl_->failed || !impl_->owner_tasks ||
        !impl_->owner_tasks->Succeed(identity)) {
        return false;
    }

    impl_->RevokeOwnerTaskScope();
    impl_->PublishOwnerTaskChange();
    return true;
}

bool ClientApplication::CancelOwnerTask(runtime::TaskIdentity identity,
                                        runtime::TaskCancelReason reason)
{
    impl_->ReconcileOwnerTask();
    if (impl_->closed || impl_->failed || !impl_->owner_tasks ||
        !impl_->owner_tasks->Cancel(identity, reason)) {
        return false;
    }

    impl_->RevokeOwnerTaskScope();
    impl_->PublishOwnerTaskChange();
    return true;
}

bool ClientApplication::FailOwnerTask(runtime::TaskIdentity identity,
                                      const runtime::TaskFailure &failure)
{
    impl_->ReconcileOwnerTask();
    if (impl_->closed || impl_->failed || !impl_->owner_tasks ||
        !impl_->owner_tasks->Fail(identity, failure)) {
        return false;
    }

    impl_->RevokeOwnerTaskScope();
    impl_->PublishOwnerTaskChange();
    return true;
}

std::optional<runtime::TaskTerminal> ClientApplication::TakeOwnerTaskTerminal()
{
    impl_->ReconcileOwnerTask();
    if (impl_->closed || impl_->failed || impl_->owner_tasks_retired || !impl_->owner_tasks) {
        return std::nullopt;
    }
    return impl_->owner_tasks->TakeTerminal();
}

void ClientApplication::RetireOwnerTasks()
{
    impl_->RetireOwnerTasks(runtime::TaskCancelReason::OwnerClosed);
}
} // namespace prism::sdk
