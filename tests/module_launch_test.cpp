#include "prism/sdk/module_session.hpp"
#include <cassert>
#include <string>
int main(int argc, char** argv) {
    assert(argc == 2);
    std::string status;
    prism::sdk::ModuleSession session(argv[1], "parent", 42,
        [&](std::string_view key, prism::runtime::PropertyValue value) {
            assert(key == "status"); status = std::get<std::string>(value); return true;
        }, [](std::string_view app) -> std::uint64_t { assert(app == "demo_player"); return 11; });
    assert(session.Start() && session.BackendReady()); assert(status == "Parent ready");
    session.Deliver({{12}, {43}, 10, prism::contracts::LaunchMilestone::BackendReady, prism::contracts::LaunchError::None, 0, {}});
    assert(status == "Parent ready"); // Unrelated request is not projected to the module.
    session.Deliver({{11}, {43}, 10, prism::contracts::LaunchMilestone::BackendReady, prism::contracts::LaunchError::None, 0, {}});
    assert(status == "Child ready");
    session.Disconnected();
}
