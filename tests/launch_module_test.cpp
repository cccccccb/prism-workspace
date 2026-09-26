#include "prism/launch/module.hpp"
#include "prism/launch/error.hpp"
#include <cassert>
#include <string_view>
#include <stdexcept>

struct State { bool playing{}, ready{}; };
int main(int argc, char** argv) {
    assert(argc == 4);
    State state;
    PrismHostApiV1 host{};
    host.struct_size = sizeof(host); host.abi_version = PRISM_APP_ABI_V1; host.context = &state;
    host.set_binding = [](void* context, PrismStringViewV1 key, PrismValueV1 value) -> int32_t {
        assert(std::string_view(key.data, key.size) == "playing" && value.kind == PRISM_VALUE_BOOL_V1);
        static_cast<State*>(context)->playing = value.as.boolean != 0;
        return 0;
    };
    host.backend_ready = [](void* context) -> int32_t { static_cast<State*>(context)->ready = true; return 0; };
    prism::launch::AppModule module(argv[1]);
    PrismAppInitV1 init{sizeof(init), PRISM_APP_ABI_V1, 1, {"music", 5}, &host};
    void* instance = module.Api().create(&init);
    assert(instance);
    module.Api().on_action(instance, {"play", 4});
    assert(state.playing && state.ready);
    module.Api().destroy(instance);
    init.abi_version = 99;
    assert(!module.Api().create(&init));
    for (int i = 2; i < argc; ++i) {
        bool rejected = false;
        try { prism::launch::AppModule invalid(argv[i]); } catch (const prism::launch::LaunchFailure& error) {
            assert(error.Code() == prism::contracts::LaunchError::UnsupportedAbi); rejected = true;
        }
        assert(rejected);
    }
    bool rejected = false;
    try { prism::launch::AppModule invalid("/nonexistent/prism-module.so"); } catch (const prism::launch::LaunchFailure& error) {
        assert(error.Code() == prism::contracts::LaunchError::ModuleLoadFailed); rejected = true;
    }
    assert(rejected);
}
