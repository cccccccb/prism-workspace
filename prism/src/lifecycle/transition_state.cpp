#include "prism/lifecycle/transition_state.hpp"
#include "prism/lifecycle/master_state.hpp"
#include "prism/wm/window.hpp"
#include "prism/core/logging.hpp"

namespace prism::lifecycle {

void TransitionState::OnEnter(wm::Window& window) {
    progress_ = 0.0f;
    PRISM_LOG_INFO("STATE", "[%s] Entered TransitionState (Mac-style Crossfade / Spring Morph)", window.GetAppId().c_str());
}

void TransitionState::Tick(wm::Window& window, float dt) {
    progress_ += dt * speed_;
    if (progress_ >= 1.0f) {
        progress_ = 1.0f;
        PRISM_LOG_INFO("STATE", "[%s] Morph complete! Destroying Preview tree and entering MasterState.", window.GetAppId().c_str());
        window.DestroyPreviewTree();
        window.TransitionTo(std::make_unique<MasterState>());
    }
}

void TransitionState::OnMasterReady(wm::Window& window) {
    (void)window;
}

} // namespace prism::lifecycle
