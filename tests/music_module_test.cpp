#include "prism/sdk/module_session.hpp"
#include <cassert>
#include <map>
#include <string>

int main(int argc, char **argv)
{
    assert(argc == 2);
    // A module that only needs the original host prefix must not require new
    // optional theme/palette APIs merely because its header was extended.
    prism::launch::AppModule compatible(argv[1]);
    PrismHostApiV1 original{};
    original.struct_size = offsetof(PrismHostApiV1, subscribe_instances);
    original.abi_version = PRISM_APP_ABI_V1;
    original.set_binding = [](void *, PrismStringViewV1, PrismValueV1) -> int32_t {
        return 0;
    };
    original.backend_ready = [](void *) -> int32_t {
        return 0;
    };
    original.schedule_tick = [](void *, uint64_t) -> int32_t {
        return 0;
    };
    PrismAppInitV1 init{sizeof(init), PRISM_APP_ABI_V1, 1, {"demo_player", 11}, &original};
    void *business = compatible.Api().create(&init);
    assert(business);
    compatible.Api().destroy(business);
    std::map<std::string, prism::runtime::PropertyValue> bindings;
    auto sink = [&](std::string_view key, prism::runtime::PropertyValue value) {
        bindings[std::string(key)] = std::move(value);
        return true;
    };
    prism::sdk::ModuleSession session(argv[1], "demo_player", 42, sink);
    assert(!session.BackendReady());
    assert(session.Start());
    assert(!session.Start());
    assert(session.BackendReady());
    assert(std::get<std::string>(bindings["track_title"]) == "Hotel California");
    assert(std::get<std::string>(bindings["track_artist"]) == "Eagles");
    assert(std::get<std::string>(bindings["playback_elapsed"]).find(':') != std::string::npos);
    assert(std::get<std::string>(bindings["playback_duration"]).find(':') != std::string::npos);
    assert(std::get<double>(bindings["playback_progress"]) == 0.42);
    assert(std::get<std::string>(bindings["playback_icon"]) == "play");
    session.Action("player:toggle");
    assert(!bindings.contains("play_state_icon"));
    assert(std::get<std::string>(bindings["playback_icon"]) == "pause");
    session.Tick(prism::sdk::MonotonicNs() + 600000000ULL);
    assert(std::get<double>(bindings["playback_progress"]) > 0.42);
    session.Action("nav:favorites");
    assert(std::get<std::string>(bindings["track_title"]) == "Starboy");
    assert(std::get<double>(bindings["playback_progress"]) == 0);
    session.Action("nav:library");
    assert(std::get<std::string>(bindings["track_title"]) == "Bohemian Rhapsody");
    assert(std::get<double>(bindings["playback_progress"]) == 0.15);
    session.Action("player:next");
    assert(std::get<std::string>(bindings["track_title"]) == "Starboy");
    assert(std::get<double>(bindings["playback_progress"]) == 0);
    session.Action("player:previous");
    assert(std::get<std::string>(bindings["track_title"]) == "Bohemian Rhapsody");
    assert(std::get<double>(bindings["playback_progress"]) == 0);
    session.Action("player:toggle");
    session.Tick(prism::sdk::MonotonicNs() + 600000000ULL);
    assert(std::get<double>(bindings["playback_progress"]) == 0);
    assert(session.TimeoutMs(prism::sdk::MonotonicNs(), 100) <= 100);
    prism::sdk::ModuleSession rejected(argv[1], "demo_player", 43,
                                       [](auto, auto) { return false; });
    assert(!rejected.Start());
    assert(!rejected.BackendReady());
    assert(!rejected.Start());
    rejected.Tick(prism::sdk::MonotonicNs() + 1000000000ULL);
    prism::sdk::ModuleSession wrong_identity(argv[1], "demo_player", 0, sink);
    assert(!wrong_identity.Start());
}
