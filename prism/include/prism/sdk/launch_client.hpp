#pragma once
#include "prism/launch/protocol.hpp"
#include <memory>
#include <string>
namespace prism::launch { class Stream; }
namespace prism::sdk {
std::string DefaultLaunchSocket();
class LaunchClient {
public:
    explicit LaunchClient(std::string socket = {});
    ~LaunchClient();
    LaunchClient(const LaunchClient&) = delete;
    LaunchClient& operator=(const LaunchClient&) = delete;
    bool Connect();
    std::uint64_t Launch(std::string app_id, contracts::LaunchMode mode = contracts::LaunchMode::ActivateOrCreate);
    std::uint64_t SubscribeInstances();
    std::vector<contracts::InstanceUpdate> TakeInstanceUpdates();
    std::uint64_t SelectTheme(std::string id, std::string color_scheme = {});
    std::vector<contracts::ThemeEvent> TakeThemeEvents();
    bool Cancel(contracts::RequestId request);
    std::vector<contracts::LaunchEvent> Pump(int timeout_ms);
    bool Connected() const;
private:
    std::vector<contracts::InstanceUpdate> updates_;
    std::vector<contracts::ThemeEvent> themes_;
    std::string socket_;
    std::unique_ptr<launch::Stream> stream_;
    std::uint64_t next_request_{1};
};
} // namespace prism::sdk
