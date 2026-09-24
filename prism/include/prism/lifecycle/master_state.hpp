#pragma once

#include "prism/lifecycle/window_state.hpp"

namespace prism::lifecycle {

class MasterState : public WindowState {
public:
    std::string GetStateName() const override { return "Master (Interactive)"; }
    void OnEnter(wm::Window& window) override;
    void Tick(wm::Window& window, float dt) override;
    void OnMasterReady(wm::Window& window) override;
};

} // namespace prism::lifecycle
