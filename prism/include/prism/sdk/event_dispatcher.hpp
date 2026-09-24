#pragma once

#include "prism/ipc/protocol.hpp"
#include <string>
#include <functional>
#include <unordered_map>

namespace prism::sdk {

using ActionHandler = std::function<void(const ipc::EventPacket&)>;

/**
 * @brief Observer / Reactor Pattern: Dispatches incoming UI actions to registered callbacks
 */
class EventDispatcher {
public:
    EventDispatcher() = default;

    void Register(const std::string& action, ActionHandler handler) {
        handlers_[action] = std::move(handler);
    }

    bool Dispatch(const ipc::EventPacket& event) {
        std::string act = event.action;
        auto it = handlers_.find(act);
        if (it != handlers_.end()) {
            it->second(event);
            return true;
        }
        return false;
    }

private:
    std::unordered_map<std::string, ActionHandler> handlers_;
};

} // namespace prism::sdk
