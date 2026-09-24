#include "dock_item.hpp"
#include "prism/core/logging.hpp"
#include <algorithm>

namespace prism::dock {

DockManager::DockManager() {
    // Populate default apps matching 1.png style
    AddItem("files", "Files", "📁", "prism-files");
    AddItem("terminal", "Terminal", ">_", "alacritty", true);
    AddItem("browser", "Browser", "🌐", "prism-browser", true);
    AddItem("editor", "Editor", "⚡", "prism-editor");
    AddItem("music", "Music", "🎵", "demo_player", true);
    AddItem("settings", "Settings", "⚙", "demo_settings");
}

void DockManager::AddItem(const std::string& id, const std::string& name, const std::string& icon, const std::string& exec_cmd, bool is_running) {
    items_.push_back({id, name, icon, exec_cmd, is_running, true});
}

bool DockManager::LaunchOrActivate(const std::string& id) {
    for (auto& item : items_) {
        if (item.id == id) {
            item.is_running = true;
            PRISM_LOG_INFO("DOCK", "Launched / Activated application [%s: '%s'] (exec: %s)",
                           item.id.c_str(), item.name.c_str(), item.exec_cmd.c_str());
            return true;
        }
    }
    return false;
}

void DockManager::SetRunning(const std::string& id, bool running) {
    for (auto& item : items_) {
        if (item.id == id) {
            item.is_running = running;
            return;
        }
    }
}

size_t DockManager::GetRunningCount() const {
    size_t count = 0;
    for (const auto& item : items_) {
        if (item.is_running) count++;
    }
    return count;
}

} // namespace prism::dock
