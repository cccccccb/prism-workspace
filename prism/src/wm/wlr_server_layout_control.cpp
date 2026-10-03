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
                   view->shell_role == static_cast<int>(permit.role) && view->mapped;
        });
    return principal;
}

void WlrServer::HandleLayoutControl(const launch::ControlMessage &message)
{
    auto reply = message;
    reply.type = launch::ControlType::LayoutControlResult;
    reply.control_result =
        layout_controls_.Apply(LayoutPrincipal(message.permit), message.control_request,
                               *GetLayoutSnapshot(), launch::MonotonicNs(), this);
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
        if (boundary_drag_ && boundary_drag_->session == delivery.result.session) {
            CancelBoundaryPreview();
        }
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
    std::uint64_t boundary{};
    boundary_pointer_.reset();
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
            if (principal.role == contracts::WindowRole::LayoutControls &&
                proof.kind == contracts::LayoutInputKind::Pointer && boundary_handle_.visible) {
                boundary = boundary_handle_.boundary;
                const auto range =
                    compositor_->GetTreeEngine().GetBoundaryRange(boundary, CurrentTreeLayout());
                if (range && range->feasible) {
                    const contracts::LogicalPoint point{cursor_->x, cursor_->y};
                    boundary_pointer_ =
                        BoundaryPointer{proof, boundary, point, point, range->position, false};
                }
            }
        }
        break;
    }
    layout_controls_.RecordInput(principal, proof, launch::MonotonicNs(), boundary);
}

void WlrServer::CancelLayoutControlsForSurface(wlr_surface *surface,
                                               contracts::LayoutControlError error)
{
    if (!surface) {
        return;
    }
    auto *root = wlr_surface_get_root_surface(surface);
    for (const auto &view : xdg_views_) {
        if (view->toplevel->base->surface == root) {
            layout_controls_.CancelInstance({view->instance}, error);
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
    std::erase_if(recovery_buttons_,
                  [binding](const auto &button) { return button.first == binding->device; });
    if (control_pointer_device_ == binding->device) {
        layout_controls_.CancelInput(contracts::LayoutInputKind::Pointer, 0);
        const auto &state = seat_->pointer_state;
        if (std::find(state.buttons, state.buttons + state.button_count, 272u) !=
            state.buttons + state.button_count) {
            // Device loss cancels the client sequence before releasing the
            // seat's implicit grab; never synthesize a client activation.
            wlr_seat_pointer_notify_clear_focus(seat_);
            wlr_seat_pointer_notify_button(
                seat_, static_cast<std::uint32_t>(launch::MonotonicNs() / 1000000), 272,
                WL_POINTER_BUTTON_STATE_RELEASED);
            wlr_seat_pointer_notify_frame(seat_);
        }
        control_pointer_device_ = nullptr;
    }
    std::erase_if(pointers_,
                  [binding](const auto &candidate) { return candidate.get() == binding; });
    UpdateGroupRecovery();
}

} // namespace prism::wm
