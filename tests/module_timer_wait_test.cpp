#include "prism/sdk/module_session.hpp"
#include <cassert>
#include <limits>

int main(int argc, char **argv)
{
    assert(argc == 2);
    double ticks = -1;
    prism::sdk::ModuleSession module(
        argv[1], "timer.fixture", 1,
        [&](std::string_view key, prism::runtime::PropertyValue value) {
            assert(key == "ticks");
            ticks = std::get<double>(value);
            return true;
        });
    assert(module.Start() && module.BackendReady() && ticks == 0);
    auto now = prism::sdk::MonotonicNs();
    assert(module.TimeoutMs(now, -1) == -1); // No timer means an interruptible indefinite wait.
    assert(module.TimeoutMs(now, 123) == 123);
    module.Tick(std::numeric_limits<std::uint64_t>::max());
    assert(ticks == 0);
    module.Action("once");
    now = prism::sdk::MonotonicNs();
    const int remaining = module.TimeoutMs(now, -1);
    assert(remaining > 0 && remaining <= 50);
    assert(module.TimeoutMs(now, 7) == 7);
    module.Tick(now);
    assert(ticks == 0);
    assert(module.TimeoutMs(now + 100000000, -1) == 0);
    module.Tick(now + 100000000);
    assert(ticks == 1);
    module.Tick(now + 100000000);
    assert(ticks == 1); // A consumed deadline never spins.
    assert(module.TimeoutMs(now + 100000000, -1) == -1);
    module.Action("repeat");
    now = prism::sdk::MonotonicNs();
    module.Tick(now + 100000000);
    assert(ticks == 2);
    // Tick clears the old deadline before invoking the module, preserving the
    // replacement deadline scheduled by its callback.
    now = prism::sdk::MonotonicNs();
    assert(module.TimeoutMs(now, -1) > 0 && module.TimeoutMs(now, -1) <= 50);
    module.Tick(now);
    assert(ticks == 2);
    module.Action("once");
    module.Tick(prism::sdk::MonotonicNs() + 100000000);
    assert(ticks == 3);
    assert(module.TimeoutMs(prism::sdk::MonotonicNs(), -1) == -1);
}
