#include "client_application_p.hpp"

namespace prism::sdk {
bool ClientApplication::RefreshOwnerTask(runtime::TaskIdentity identity)
{
    auto &app = *impl_;
    app.ReconcileOwnerTask();
    const auto active = ActiveOwnerTask();
    if (app.closed || app.failed || !active || active->identity != identity ||
        !app.owner_task_scope || app.owner_task_scope->identity != identity || !app.scene ||
        app.owner_task_scope->ui != app.installed_ui) {
        return false;
    }

    app.scene->ResolveLayout();
    const auto token = app.scene->RefreshOwnerModal(app.owner_task_scope->token);
    if (!token) {
        app.owner_tasks->Fail(identity, {runtime::TaskFailureCode::PreparationFailed,
                                         "Task input projection could not be refreshed"});
        app.RevokeOwnerTaskScope();
        app.PublishOwnerTaskChange();
        return false;
    }
    app.owner_task_scope->token = *token;
    if (!app.owner_task_presentation.Reproject(app.owner_task_scope->presentation, *token,
                                               app.scene->OwnerModalEpoch(),
                                               app.owner_task_scope->root)) {
        app.owner_tasks->Fail(identity, {runtime::TaskFailureCode::PreparationFailed,
                                         "Task presentation projection could not be refreshed"});
        app.RevokeOwnerTaskScope();
        app.PublishOwnerTaskChange();
        return false;
    }
    app.owner_tasks->Reprepare(identity);

    try {
        app.PublishOwnerTaskChange();
    } catch (...) {
        if (app.owner_tasks->Cancel(identity, runtime::TaskCancelReason::ScopeUnavailable) &&
            app.owner_task_scope && app.owner_task_scope->identity == identity) {
            app.RevokeOwnerTaskScope();
        }
        throw;
    }
    return app.owner_task_scope && app.owner_task_scope->identity == identity;
}
} // namespace prism::sdk
