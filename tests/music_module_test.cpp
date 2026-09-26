#include "prism/sdk/module_session.hpp"
#include <cassert>
#include <map>
#include <string>

int main(int argc, char** argv) {
    assert(argc == 2);
    std::map<std::string, prism::runtime::PropertyValue> bindings;
    auto sink = [&](std::string_view key, prism::runtime::PropertyValue value) {
        bindings[std::string(key)] = std::move(value); return true;
    };
    prism::sdk::ModuleSession session(argv[1], "demo_player", 42, sink);
    assert(!session.BackendReady());
    assert(session.Start());
    assert(!session.Start());
    assert(session.BackendReady());
    assert(std::get<std::string>(bindings["track_title"]) == "Hotel California - Eagles");
    assert(std::get<std::string>(bindings["playback_progress_text"]) == "Progress: 42%");
    assert(std::get<double>(bindings["playback_progress"]) == 0.42);
    assert(std::get<std::string>(bindings["playback_icon"]) == "play");
    session.Action("player:toggle");
    assert(std::get<std::string>(bindings["play_state_icon"]) == "Pause");
    assert(std::get<std::string>(bindings["playback_icon"]) == "pause");
    session.Tick(prism::sdk::MonotonicNs() + 600000000ULL);
    assert(std::get<std::string>(bindings["playback_progress_text"]) == "Progress: 44%");
    session.Action("nav:favorites");
    assert(std::get<std::string>(bindings["track_title"]) == "Starboy - The Weeknd");
    assert(std::get<std::string>(bindings["playback_progress_text"]) == "Progress: 0%");
    session.Action("nav:library");
    assert(std::get<std::string>(bindings["track_title"]) == "Bohemian Rhapsody - Queen");
    assert(std::get<std::string>(bindings["playback_progress_text"]) == "Progress: 15%");
    session.Action("player:next");
    assert(std::get<std::string>(bindings["track_title"]) == "Starboy - The Weeknd");
    assert(std::get<double>(bindings["playback_progress"]) == 0);
    session.Action("player:previous");
    assert(std::get<std::string>(bindings["track_title"]) == "Bohemian Rhapsody - Queen");
    assert(std::get<double>(bindings["playback_progress"]) == 0);
    session.Action("player:toggle");
    session.Tick(prism::sdk::MonotonicNs() + 600000000ULL);
    assert(std::get<std::string>(bindings["playback_progress_text"]) == "Progress: 0%");
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
