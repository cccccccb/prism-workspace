#include "service_p.hpp"
#include <iostream>
#include <stdexcept>

namespace prism::launcher {
using detail::Now;
using detail::Require;

void Service::Impl::SendControl(launch::ControlType type, const Job &job, std::uint64_t transaction)
{
    if (!control || !session) {
        return;
    }
    launch::ControlMessage message;
    message.type = type;
    auto &p = message.permit;
    p.session = session;
    p.request = {transaction ? transaction : job.request.request.value};
    p.instance = job.instance;
    p.pid = job.state.Pid();
    p.role = job.role;
    if (type == launch::ControlType::Grant) {
        launch::RandomBytes(p.token);
        p.expires_ns = Now() + 5000000000ULL;
    }
    if (!control->Queue(launch::EncodeControl(message))) {
        control_failed = true;
    }
}

void Service::Impl::ReadControl()
{
    if (!control) {
        return;
    }
    try {
        for (const auto &frame : control->Receive()) {
            const auto m = launch::DecodeControl(frame);
            const auto &p = m.permit;
            if (m.type == launch::ControlType::Ready) {
                Require(!session && m.success, "Unexpected WM ready");
                session = p.session;
                SendWmTheme();
                continue;
            }
            Require(session && p.session == session, "WM session mismatch");
            if (m.type == launch::ControlType::ThemeApplied) {
                Require(m.theme_applied.generation <= theme.generation, "Unexpected WM theme ACK");
                if (m.theme_applied.generation != theme.generation) {
                    continue;
                }
                if (!m.theme_applied.success) {
                    RollbackTheme("WM rejected theme: " + m.theme_applied.detail);
                    continue;
                }
                if (!theme_ready) {
                    theme_ready = true;
                    if (config.start_shell) {
                        BootstrapShell();
                    }
                }
                if (theme_transaction) {
                    theme_transaction->wm_pending = false;
                    FinishTheme();
                }
                continue;
            }
            if (m.type == launch::ControlType::Registered ||
                m.type == launch::ControlType::Activated) {
                auto it = jobs.find(p.request.value);
                Require(it != jobs.end(), "Unknown WM transaction");
                auto &job = *it->second;
                Require(p.instance == job.instance && p.pid == job.state.Pid() &&
                            p.role == job.role,
                        "WM identity mismatch");
                if (job.state.Terminal()) {
                    continue;
                }
                if (!m.success) {
                    Fail(job, LaunchError::RuntimeFailed, "WM rejected registration or activation");
                    continue;
                }
                if (m.type == launch::ControlType::Registered) {
                    Require(!job.alias && !job.registered, "Duplicate registration ACK");
                    job.registered = true;
                    // Binding is gated in Maintain by the current acknowledged theme.
                } else {
                    Require(job.alias && job.activation_sent, "Unexpected activation ACK");
                    Event(job, LaunchMilestone::Activated);
                }
            } else if (m.type == launch::ControlType::Mapped ||
                       m.type == launch::ControlType::Unmapped) {
                Job *found = nullptr;
                for (auto &[id, job] : jobs) {
                    if (!job->alias && job->instance == p.instance && job->state.Pid() == p.pid) {
                        found = job.get();
                        break;
                    }
                }
                Require(found && found->role == p.role, "Unknown WM window");
                WindowChanged(*found, m.type == launch::ControlType::Mapped);
            } else {
                throw std::runtime_error("Unexpected WM reply");
            }
        }
        if (control->Closed()) {
            control_failed = true;
        }
    } catch (const std::exception &error) {
        std::cerr << "WM control failed: " << error.what() << std::endl;
        control_failed = true;
    }
}

} // namespace prism::launcher
