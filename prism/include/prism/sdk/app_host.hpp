#pragma once
#include "prism/contracts/launch.hpp"
#include "prism/launch/package.hpp"
#include <functional>
#include <memory>

namespace prism::sdk {
struct HostConfig {
    std::string socket;
    std::string font_path{"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
    contracts::RequestId request{1};
    contracts::InstanceId instance{1};
    std::function<void(const contracts::LaunchEvent&)> on_event;
    std::function<std::uint64_t(std::string_view)> launch_app;
    std::function<std::uint64_t()> subscribe_instances;
};
// Constructible in a single-thread seed. PrepareFrontend must run in the final
// worker: it creates resource threads. Bind creates the application surface/GPU.
class AppHost {
public:
    explicit AppHost(HostConfig config);
    ~AppHost();
    AppHost(const AppHost&) = delete;
    AppHost& operator=(const AppHost&) = delete;
    bool PrepareFrontend();
    bool Assign(contracts::RequestId request, contracts::InstanceId instance);
    void DeliverLaunchEvent(const contracts::LaunchEvent& event);
    void DeliverInstanceEvent(const contracts::InstanceUpdate& event);
    bool Bind(const launch::AppPackage& package);
    bool Pump(int timeout_ms);
    bool IsCloseRequested() const;
    void Close();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace prism::sdk
