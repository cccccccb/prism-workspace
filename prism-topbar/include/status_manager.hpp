#pragma once

#include <string>
#include <chrono>

namespace prism::topbar {

struct SystemStatus {
    std::string time_str;
    std::string network_str;
    std::string battery_str;
    std::string notification_str;
    std::string active_workspace;
    int unread_notifications{2};
    bool is_charging{true};
    int battery_percent{100};
};

class StatusManager {
public:
    StatusManager();

    // Refresh time and status readings
    void Update();

    const SystemStatus& GetStatus() const { return status_; }
    void ToggleNotifications();
    void SetActiveWorkspace(const std::string& ws_name);

private:
    SystemStatus status_;
};

} // namespace prism::topbar
