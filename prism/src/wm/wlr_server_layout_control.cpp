#include "wlr_server_internal.hpp"

namespace prism::wm {

LayoutControlPrincipal WlrServer::LayoutPrincipal(const launch::ShellPermit &permit) const
{
    LayoutControlPrincipal principal{permit.instance, permit.request, permit.pid, permit.role,
                                     false};
    const auto registration = registrations_.find(permit.pid);
    if (registration == registrations_.end()) {
        return principal;
    }
    const auto &registered = registration->second->permit;
    if (registered.instance != permit.instance || registered.role != permit.role ||
        registered.request != permit.request || registered.session != control_session_) {
        return principal;
    }

    pollfd process{registration->second->pidfd, POLLIN, 0};
    if (process.fd < 0 || poll(&process, 1, 0) != 0) {
        return principal;
    }
    principal.mapped =
        std::any_of(xdg_views_.begin(), xdg_views_.end(), [&permit](const auto &view) {
            return view->instance == permit.instance.value &&
                   view->pid == static_cast<pid_t>(permit.pid) &&
                   view->shell_role == static_cast<int>(permit.role) && view->mapped &&
                   view->visible;
        });
    return principal;
}

void WlrServer::HandleLayoutControl(const launch::ControlMessage &message)
{
    auto reply = message;
    reply.type = launch::ControlType::LayoutControlResult;
    reply.control_result =
        layout_controls_.Apply(LayoutPrincipal(message.permit), message.control_request,
                               *GetLayoutSnapshot(), launch::MonotonicNs());
    reply.control_request = {};
    reply.success = reply.control_result.error == contracts::LayoutControlError::None;
    // Send automatic invalidations before the response to the next request.
    PublishLayoutControlNotifications();
    if (!control_->Queue(launch::EncodeControl(reply))) {
        throw std::runtime_error("WM layout control queue overflow");
    }
}

void WlrServer::PublishLayoutControlNotifications()
{
    for (const auto &delivery : layout_controls_.TakeNotifications()) {
        if (!control_ || control_failed_) {
            continue;
        }
        launch::ControlMessage message;
        message.type = launch::ControlType::LayoutControlResult;
        message.permit.session = control_session_;
        message.permit.instance = delivery.principal.instance;
        message.permit.request = delivery.principal.launch_request;
        message.permit.pid = delivery.principal.pid;
        message.permit.role = delivery.principal.role;
        message.control_result = delivery.result;
        if (!control_->Queue(launch::EncodeControl(message))) {
            throw std::runtime_error("WM layout cancellation queue overflow");
        }
    }
}

void WlrServer::RecordLayoutInput(wlr_surface *surface, contracts::LayoutInputProof proof)
{
    LayoutControlPrincipal principal;
    auto *root = surface ? wlr_surface_get_root_surface(surface) : nullptr;
    for (const auto &view : xdg_views_) {
        if (view->toplevel->base->surface != root) {
            continue;
        }
        const auto registration = registrations_.find(view->pid);
        if (registration != registrations_.end() && view->mapped && view->visible &&
            view->instance == registration->second->permit.instance.value &&
            view->shell_role == static_cast<int>(registration->second->permit.role)) {
            principal = LayoutPrincipal(registration->second->permit);
        }
        break;
    }
    layout_controls_.RecordInput(principal, proof, launch::MonotonicNs());
}

void WlrServer::CancelLayoutControlsForSurface(wlr_surface *surface)
{
    if (!surface) {
        return;
    }
    auto *root = wlr_surface_get_root_surface(surface);
    for (const auto &view : xdg_views_) {
        if (view->toplevel->base->surface == root) {
            layout_controls_.Revoke({view->instance});
        }
    }
}

WlrPointerBinding::WlrPointerBinding(WlrServer *owner, wlr_input_device *input)
    : server(owner), device(input)
{
    destroy.notify = Destroy;
    wl_signal_add(&device->events.destroy, &destroy);
}

WlrPointerBinding::~WlrPointerBinding()
{
    wl_list_remove(&destroy.link);
}

void WlrPointerBinding::Destroy(wl_listener *listener, void *)
{
    auto *binding =
        WlContainerOf<WlrPointerBinding>(listener, offsetof(WlrPointerBinding, destroy));
    binding->server->HandlePointerDestroy(binding);
}

void WlrServer::HandlePointerDestroy(WlrPointerBinding *binding)
{
    if (control_pointer_device_ == binding->device) {
        layout_controls_.CancelInput(contracts::LayoutInputKind::Pointer, 0);
        control_pointer_device_ = nullptr;
    }
    std::erase_if(pointers_,
                  [binding](const auto &candidate) { return candidate.get() == binding; });
}

} // namespace prism::wm
