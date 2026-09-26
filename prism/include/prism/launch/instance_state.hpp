#pragma once
#include "prism/contracts/launch.hpp"
#include <optional>

namespace prism::launch {
// One assigned launch, monotonic milestones; BackendReady is orthogonal.
// Invalid/duplicate/late events leave the previous state unchanged.
class InstanceState {
public:
    InstanceState(contracts::RequestId request, contracts::InstanceId instance);
    bool Apply(const contracts::LaunchEvent& event);
    bool Accepted() const { return accepted_; }
    bool RuntimeReady() const { return runtime_ready_; }
    bool FirstPresented() const { return presented_; }
    bool BackendReady() const { return backend_ready_; }
    bool Activated() const { return activated_; }
    bool Terminal() const { return failed_ || exited_; }
    std::uint32_t Pid() const { return pid_; }
    std::optional<std::int32_t> ExitCode() const {
        return exited_ ? std::optional{exit_code_} : std::nullopt;
    }
    contracts::LaunchError Error() const { return error_; }
private:
    contracts::RequestId request_;
    contracts::InstanceId instance_;
    std::uint32_t pid_{};
    std::int32_t exit_code_{};
    bool accepted_{}, assigned_{}, runtime_ready_{}, configured_{}, presented_{};
    bool activated_{}, backend_ready_{}, failed_{}, exited_{};
    contracts::LaunchError error_{contracts::LaunchError::None};
};
} // namespace prism::launch
