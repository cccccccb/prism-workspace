#include "prism/launch/instance_state.hpp"
#include <stdexcept>

namespace prism::launch {
InstanceState::InstanceState(contracts::RequestId request, contracts::InstanceId instance)
    : request_(request), instance_(instance)
{
    if (!request.value || !instance.value) {
        throw std::invalid_argument("Instance requires nonzero launch IDs");
    }
}

bool InstanceState::Apply(const contracts::LaunchEvent &event)
{
    using contracts::LaunchError;
    using contracts::LaunchMilestone;
    if (exited_ || event.request != request_ || event.instance != instance_) {
        return false;
    }
    if (static_cast<unsigned>(event.milestone) >
            static_cast<unsigned>(LaunchMilestone::Activated) ||
        static_cast<unsigned>(event.error) > static_cast<unsigned>(LaunchError::SessionEnded)) {
        return false;
    }
    if (activated_ && event.milestone != LaunchMilestone::Exited &&
        event.milestone != LaunchMilestone::Failed) {
        return false;
    }
    if (failed_ && event.milestone != LaunchMilestone::Exited) {
        return false;
    }
    if (event.milestone != LaunchMilestone::Exited && event.exit_code != 0) {
        return false;
    }
    if (event.exit_code < -64 || event.exit_code > 255) {
        return false;
    }
    if (event.milestone == LaunchMilestone::Failed) {
        if (event.error == LaunchError::None || event.pid != pid_) {
            return false;
        }
        failed_ = true;
        error_ = event.error;
        return true;
    }
    if (event.error != LaunchError::None) {
        return false;
    }
    switch (event.milestone) {
    case LaunchMilestone::Accepted:
        if (accepted_ || event.pid) {
            return false;
        }
        accepted_ = true;
        return true;
    case LaunchMilestone::WorkerAssigned:
        if (!accepted_ || assigned_ || !event.pid) {
            return false;
        }
        pid_ = event.pid;
        assigned_ = true;
        return true;
    default:
        break;
    }
    if (!assigned_ || event.pid != pid_) {
        return false;
    }
    switch (event.milestone) {
    case LaunchMilestone::RuntimeReady:
        if (runtime_ready_) {
            return false;
        }
        runtime_ready_ = true;
        return true;
    case LaunchMilestone::SurfaceConfigured:
        if (!runtime_ready_ || configured_) {
            return false;
        }
        configured_ = true;
        return true;
    case LaunchMilestone::FirstPresented:
        if (!configured_ || presented_) {
            return false;
        }
        presented_ = true;
        return true;
    case LaunchMilestone::BackendReady:
        if (!runtime_ready_ || backend_ready_) {
            return false;
        }
        backend_ready_ = true;
        return true;
    case LaunchMilestone::Activated:
        if (activated_ || runtime_ready_) {
            return false;
        }
        activated_ = true;
        return true;
    case LaunchMilestone::Exited:
        exited_ = true;
        exit_code_ = event.exit_code;
        return true;
    default:
        return false;
    }
}
} // namespace prism::launch
