#include "status_manager.hpp"
#include <ctime>
#include <iomanip>
#include <sstream>

namespace prism::topbar {

StatusManager::StatusManager() {
    status_.active_workspace = "Main";
    status_.network_str = "Wi-Fi 5G";
    status_.battery_percent = 100;
    status_.is_charging = true;
    status_.unread_notifications = 2;
    Update();
}

void StatusManager::Update() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&now_c, &tm_buf);

    // Format matching 1.png: 'Oct-11 15:13:24' or current 'Sep-24 15:26:12'
    char time_str[64];
    std::strftime(time_str, sizeof(time_str), "%b-%d %H:%M:%S", &tm_buf);
    status_.time_str = time_str;

    // Battery string
    status_.battery_str = std::to_string(status_.battery_percent) + "%" + (status_.is_charging ? " ⚡" : "");

    // Notification string
    status_.notification_str = status_.unread_notifications > 0 ?
                               ("🔔 " + std::to_string(status_.unread_notifications)) : "🔔";
}

void StatusManager::ToggleNotifications() {
    if (status_.unread_notifications > 0) {
        status_.unread_notifications = 0;
    } else {
        status_.unread_notifications = 3;
    }
    Update();
}

void StatusManager::SetActiveWorkspace(const std::string& ws_name) {
    status_.active_workspace = ws_name;
}

} // namespace prism::topbar
