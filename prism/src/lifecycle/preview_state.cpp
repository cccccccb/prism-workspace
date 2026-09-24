#include "prism/lifecycle/preview_state.hpp"
#include "prism/lifecycle/transition_state.hpp"
#include "prism/wm/window.hpp"
#include "prism/core/logging.hpp"

namespace prism::lifecycle {

void PreviewState::OnEnter(wm::Window& window) {
    PRISM_LOG_INFO("STATE", "[%s] Entered PreviewState (0ms Splash screen mounted)", window.GetAppId().c_str());
}

void PreviewState::Tick(wm::Window& window, float dt) {
    // Stepping skeleton/splash shimmer
    (void)window;
    (void)dt;
}

void PreviewState::OnMasterReady(wm::Window& window) {
    PRISM_LOG_INFO("STATE", "[%s] Master backend is ready! Triggering transition to TransitionState...", window.GetAppId().c_str());
    window.TransitionTo(std::make_unique<TransitionState>());
}

} // namespace prism::lifecycle
