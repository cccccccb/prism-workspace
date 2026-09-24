#pragma once

#include <string>

namespace prism::wm {
    class Window;
}

namespace prism::lifecycle {

/**
 * @brief State Pattern: Manages transitions between Preview, Transition, and Master phases
 */
class WindowState {
public:
    virtual ~WindowState() = default;

    virtual std::string GetStateName() const = 0;
    virtual void OnEnter(wm::Window& window) = 0;
    virtual void Tick(wm::Window& window, float dt) = 0;
    virtual void OnMasterReady(wm::Window& window) = 0;
};

} // namespace prism::lifecycle
