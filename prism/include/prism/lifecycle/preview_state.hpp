#pragma once

#include "prism/lifecycle/window_state.hpp"

namespace prism::lifecycle {

class PreviewState : public WindowState {
public:
    std::string GetStateName() const override { return "Preview (0ms Splash)"; }
    void OnEnter(wm::Window& window) override;
    void Tick(wm::Window& window, float dt) override;
    void OnMasterReady(wm::Window& window) override;
};

} // namespace prism::lifecycle
