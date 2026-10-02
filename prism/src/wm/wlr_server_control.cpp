#include "wlr_server_internal.hpp"

namespace prism::wm {
WlrServer::Registration::~Registration()
{
    if (pidfd >= 0) {
        close(pidfd);
    }
}

void WlrServer::AttachControl(int fd, int parent_pid)
{
    launch::VerifyControlPeer(fd, parent_pid);
    control_ = std::make_unique<launch::Stream>(fd, launch::ControlFrameSize);
    launch::ControlMessage ready;
    ready.permit.session = control_session_;
    ready.permit.pid = getpid();
    ready.success = true;
    control_->Queue(launch::EncodeControl(ready));
    control_->Flush();
}

void WlrServer::NotifyView(WlrXdgView *view, launch::ControlType type)
{
    if (!control_ || !view->instance) {
        return;
    }
    launch::ControlMessage message;
    message.type = type;
    message.permit.session = control_session_;
    message.permit.instance = {view->instance};
    message.permit.pid = view->pid;
    message.permit.role = static_cast<contracts::WindowRole>(view->shell_role);
    message.success = true;
    if (!control_->Queue(launch::EncodeControl(message))) {
        control_failed_ = true;
    }
}

void WlrServer::PumpControl()
{
    if (!control_) {
        return;
    }
    try {
        for (const auto &frame : control_->Receive()) {
            auto m = launch::DecodeControl(frame);
            const auto &p = m.permit;
            if (p.session != control_session_) {
                throw std::runtime_error("Wrong WM session");
            }
            if (m.type == launch::ControlType::LayoutControl) {
                HandleLayoutControl(m);
            } else if (m.type == launch::ControlType::LayoutSubscribe) {
                layout_subscribed_ = m.layout_subscribe;
                layout_sent_revision_ = 0;
            } else if (m.type == launch::ControlType::InstallTheme) {
                m.type = launch::ControlType::ThemeApplied;
                m.theme_applied = InstallTheme(m.theme);
                m.theme = {};
                m.success = m.theme_applied.success;
                if (!control_->Queue(launch::EncodeControl(m))) {
                    control_failed_ = true;
                }
            } else if (m.type == launch::ControlType::Grant) {
                bool occupied = false;
                for (const auto &[pid, r] : registrations_) {
                    if (r->permit.instance == p.instance ||
                        (p.role != contracts::WindowRole::Toplevel && r->permit.role == p.role)) {
                        occupied = true;
                    }
                }
                m.type = launch::ControlType::Registered;
                m.success = false;
                if (p.pid && p.request.value && p.instance.value && !occupied &&
                    !registrations_.contains(p.pid) && p.expires_ns > launch::MonotonicNs()) {
                    auto r = std::make_unique<Registration>();
                    r->permit = p;
                    r->pidfd = syscall(SYS_pidfd_open, p.pid, 0);
                    pollfd dead{r->pidfd, POLLIN, 0};
                    if (r->pidfd >= 0 && poll(&dead, 1, 0) == 0) {
                        if (p.role != contracts::WindowRole::Toplevel) {
                            r->guard = std::make_unique<launch::ShellPermitGuard>(p);
                        }
                        registrations_.emplace(p.pid, std::move(r));
                        m.success = true;
                    }
                }
                control_->Queue(launch::EncodeControl(m));
            } else if (m.type == launch::ControlType::Revoke) {
                auto r = registrations_.find(p.pid);
                if (r != registrations_.end() && r->second->permit.instance == p.instance) {
                    const auto role = static_cast<int>(r->second->permit.role);
                    layout_controls_.Revoke(p.instance);
                    registrations_.erase(r);
                    HandleShellUnavailable(role);
                    wl_client *client = nullptr;
                    for (const auto &view : xdg_views_) {
                        if (view->instance == p.instance.value &&
                            view->pid == static_cast<pid_t>(p.pid)) {
                            client =
                                wl_resource_get_client(view->toplevel->base->surface->resource);
                            break;
                        }
                    }
                    if (client) {
                        wl_client_destroy(client);
                    }
                }
            } else if (m.type == launch::ControlType::Activate) {
                m.type = launch::ControlType::Activated;
                m.success = false;
                for (const auto &view : xdg_views_) {
                    if (view->instance == p.instance.value &&
                        view->pid == static_cast<pid_t>(p.pid) && view->mapped &&
                        !view->shell_role) {
                        FocusXdgView(view.get());
                        m.success = true;
                        break;
                    }
                }
                control_->Queue(launch::EncodeControl(m));
            } else {
                throw std::runtime_error("Unexpected launcher control message");
            }
        }
        control_->Flush();
        if (control_->Closed()) {
            control_failed_ = true;
        }
    } catch (const std::exception &error) {
        PRISM_LOG_ERROR("WLR-CONTROL", "%s", error.what());
        control_failed_ = true;
        control_->Close();
    }
    if (control_failed_) {
        layout_controls_.Reset();
    }
}

contracts::ThemeApplied WlrServer::InstallTheme(const contracts::ThemeSnapshot &snapshot)
{
    contracts::ThemeApplied applied{snapshot.generation, false, {}};
    std::shared_ptr<const contracts::ThemeSnapshot> next;
    try {
        if (!snapshot.generation) {
            throw std::invalid_argument("Theme generation must be nonzero");
        }
        // Also enforce the serialized payload bound on direct trusted callers.
        contracts::EncodeTheme(snapshot);
        if (theme_snapshot_ && snapshot.generation <= theme_snapshot_->generation) {
            if (snapshot == *theme_snapshot_) {
                return {snapshot.generation, true, "Already installed"};
            }
            throw std::invalid_argument("Theme generation must increase");
        }
        next = std::make_shared<const contracts::ThemeSnapshot>(snapshot);
    } catch (const std::exception &error) {
        applied.detail = error.what();
        return applied;
    }
    theme_.layout = next->layout;
    theme_snapshot_ = std::move(next);
    recovery_visible_ = false;
    ArrangeXdgViews();
    applied.success = true;
    applied.detail = "Installed";
    return applied;
}

} // namespace prism::wm
