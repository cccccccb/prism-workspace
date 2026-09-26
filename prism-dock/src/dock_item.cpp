#include "dock_item.hpp"
#include <algorithm>
#include <filesystem>
#include <spawn.h>

extern char** environ;

namespace prism::dock {

DockManager::DockManager() {
    // Populate default apps matching 1.png style
    AddItem("files", "Files", "📁", "prism-files");
    AddItem("terminal", "Terminal", ">_", "alacritty");
    AddItem("browser", "Browser", "🌐", "prism-browser");
    AddItem("editor", "Editor", "⚡", "prism-editor");
    AddItem("music", "Music", "🎵", "demo_player");
    AddItem("settings", "Settings", "⚙", "demo_settings");
}

void DockManager::AddItem(const std::string& id, const std::string& name, const std::string& icon, const std::string& exec_cmd, bool is_running) {
    items_.push_back({id, name, icon, exec_cmd, is_running, true});
}

bool DockManager::LaunchOrActivate(const std::string& id) {
    for (auto& item : items_) {
        if (item.id == id) {
            std::string command = item.exec_cmd;
            if (command == "demo_player" || command == "demo_settings") {
                const auto executable_dir = std::filesystem::canonical("/proc/self/exe").parent_path();
                const auto demo = executable_dir.parent_path() / "demos" / command;
                if (std::filesystem::exists(demo)) command = demo.string();
            }
            pid_t child = -1;
            char* args[] = {command.data(), nullptr};
            if (posix_spawnp(&child, command.c_str(), nullptr, nullptr, args, environ) != 0)
                return false;
            item.is_running = true;
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
