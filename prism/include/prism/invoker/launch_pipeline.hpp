#pragma once

#include <string>
#include <memory>

namespace prism::invoker {

struct LaunchContext {
    std::string app_name;
    std::string package_path;
    std::string manifest_path;
    std::string preview_path;
    std::string master_path;
    std::string channel_name;
    std::string zygote_socket;
    pid_t spawned_pid{0};
};

/**
 * @brief Chain of Responsibility Pattern: Orchestrates app launch verification and execution
 */
class LaunchStep {
public:
    virtual ~LaunchStep() = default;

    std::shared_ptr<LaunchStep> SetNext(std::shared_ptr<LaunchStep> next) {
        next_step_ = next;
        return next;
    }

    bool Handle(LaunchContext& ctx) {
        if (!Execute(ctx)) {
            return false;
        }
        if (next_step_) {
            return next_step_->Handle(ctx);
        }
        return true;
    }

protected:
    virtual bool Execute(LaunchContext& ctx) = 0;

private:
    std::shared_ptr<LaunchStep> next_step_;
};

class ManifestValidationStep : public LaunchStep {
protected:
    bool Execute(LaunchContext& ctx) override;
};

class SandboxSecurityStep : public LaunchStep {
protected:
    bool Execute(LaunchContext& ctx) override;
};

class PreviewMountStep : public LaunchStep {
protected:
    bool Execute(LaunchContext& ctx) override;
};

class ZygoteDispatchStep : public LaunchStep {
protected:
    bool Execute(LaunchContext& ctx) override;
};

} // namespace prism::invoker
