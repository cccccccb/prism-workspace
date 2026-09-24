#pragma once

#include <string>
#include <vector>

namespace prism::dock {

struct DockItem {
    std::string id;
    std::string name;
    std::string icon;
    std::string exec_cmd;
    bool is_running{false};
    bool is_pinned{true};
};

class DockManager {
public:
    DockManager();

    void AddItem(const std::string& id, const std::string& name, const std::string& icon, const std::string& exec_cmd, bool is_running = false);

    bool LaunchOrActivate(const std::string& id);
    void SetRunning(const std::string& id, bool running);

    const std::vector<DockItem>& GetItems() const { return items_; }
    size_t GetRunningCount() const;

private:
    std::vector<DockItem> items_;
};

} // namespace prism::dock
