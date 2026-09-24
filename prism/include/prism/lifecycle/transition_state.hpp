#pragma once

#include "prism/lifecycle/window_state.hpp"

namespace prism::lifecycle {

class TransitionState : public WindowState {
public:
    std::string GetStateName() const override { return "Transition (Spring Morph)"; }
    void OnEnter(wm::Window& window) override;
    void Tick(wm::Window& window, float dt) override;
    void OnMasterReady(wm::Window& window) override;

    float GetProgress() const { return progress_; }

private:
    float progress_{0.0f}; // 0.0f -> 1.0f
    float speed_{3.0f};    // ~330ms duration
};

} // namespace prism::lifecycle
