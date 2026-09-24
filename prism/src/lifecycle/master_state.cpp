#include "prism/lifecycle/master_state.hpp"
#include "prism/wm/window.hpp"
#include "prism/core/logging.hpp"

namespace prism::lifecycle {

void MasterState::OnEnter(wm::Window& window) {
    PRISM_LOG_INFO("STATE", "[%s] Entered MasterState: Full interactive control active", window.GetAppId().c_str());
}

void MasterState::Tick(wm::Window& window, float dt) {
    (void)window;
    (void)dt;
}

void MasterState::OnMasterReady(wm::Window& window) {
    (void)window;
}

} // namespace prism::lifecycle
