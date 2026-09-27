#include "prism/app/module_support.hpp"
#include "prism/sdk/module_session.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <poll.h>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {
int32_t AcceptBinding(void *, PrismStringViewV1, PrismValueV1)
{
    return 0;
}

int32_t AcceptReady(void *)
{
    return 0;
}

int32_t AcceptTick(void *, uint64_t)
{
    return 0;
}

int32_t AcceptWork(void *, const PrismWorkRequestV1 *)
{
    return PRISM_WORK_ACCEPTED_V1;
}

int32_t AcceptCancel(void *, uint64_t)
{
    return PRISM_WORK_ACCEPTED_V1;
}

void CheckCapabilities(const char *file, const std::filesystem::path &assets)
{
    PrismHostApiV1 original{};
    original.struct_size = offsetof(PrismHostApiV1, subscribe_instances);
    original.abi_version = PRISM_APP_ABI_V1;
    original.set_binding = AcceptBinding;
    original.backend_ready = AcceptReady;
    original.schedule_tick = AcceptTick;
    PrismAppInitV1 init{offsetof(PrismAppInitV1, assets_root),
                        PRISM_APP_ABI_V1,
                        1,
                        {"demo_player", 11},
                        &original,
                        {}};
    // The generic helper still accepts a complete original Host/init prefix.
    // Music requires additional capabilities and explicitly refuses it.
    assert(prism::app::ValidHost(&init));
    prism::launch::AppModule module(file);
    assert(module.Api().create(&init) == nullptr);

    const auto directory = assets.string();
    init.struct_size = sizeof(init);
    init.assets_root = {directory.data(), directory.size()};
    assert(module.Api().create(&init) == nullptr); // Missing work API tail.

    auto extended = original;
    extended.struct_size = sizeof(extended);
    extended.submit_work = AcceptWork;
    init.host = &extended;
    assert(module.Api().create(&init) == nullptr); // Missing cancel capability.
    extended.cancel_work = AcceptCancel;
    init.assets_root = {};
    assert(module.Api().create(&init) == nullptr); // Missing package directory.
    init.assets_root = {directory.data(), directory.size()};
    init.struct_size = offsetof(PrismAppInitV1, assets_root) + sizeof(init.assets_root) - 1;
    assert(module.Api().create(&init) == nullptr); // Partial optional directory tail.
}

struct Bindings {
    const std::thread::id owner{std::this_thread::get_id()};
    std::map<std::string, prism::runtime::PropertyValue, std::less<>> values;
    bool accept{true};

    bool Set(std::string_view key, prism::runtime::PropertyValue value)
    {
        assert(std::this_thread::get_id() == owner);
        if (!accept) {
            return false;
        }
        values.insert_or_assign(std::string(key), std::move(value));
        return true;
    }

    const std::string &Text(std::string_view key) const
    {
        return std::get<std::string>(values.at(std::string(key)));
    }

    double Number(std::string_view key) const
    {
        return std::get<double>(values.at(std::string(key)));
    }
};

void AwaitReady(prism::sdk::ModuleSession &session)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!session.BackendReady()) {
        assert(session.WorkPending() && std::chrono::steady_clock::now() < deadline);
        pollfd source{session.WorkCompletionFd(), POLLIN, 0};
        assert(poll(&source, 1, 5000) == 1 && (source.revents & POLLIN));
        assert(session.DispatchWork() > 0);
    }
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2);
    const auto assets = std::filesystem::canonical(argv[1]).parent_path() / "assets";
    assert(std::filesystem::is_regular_file(assets / "catalog.json"));
    CheckCapabilities(argv[1], assets);

    Bindings bindings;
    const auto sink = std::bind_front(&Bindings::Set, &bindings);
    prism::sdk::ModuleSession session(argv[1], "demo_player", 42, sink, {}, {}, {}, {}, {}, {},
                                      assets);
    assert(!session.BackendReady());
    assert(session.Start() && !session.BackendReady() && session.WorkPending());
    assert(!session.Start());
    AwaitReady(session);
    assert(bindings.Text("track_title") == "Hotel California");
    assert(bindings.Text("track_artist") == "Eagles");
    assert(bindings.Text("playback_elapsed").find(':') != std::string::npos);
    assert(bindings.Text("playback_duration").find(':') != std::string::npos);
    assert(bindings.Number("playback_progress") == 0.42);
    assert(bindings.Text("playback_icon") == "play");
    session.Action("player:toggle");
    assert(!bindings.values.contains("play_state_icon"));
    assert(bindings.Text("playback_icon") == "pause");
    session.Tick(prism::sdk::MonotonicNs() + 600000000ULL);
    assert(bindings.Number("playback_progress") > 0.42);

    session.Action("nav:favorites");
    assert(std::get<bool>(bindings.values.at("library_visible")));
    assert(bindings.Text("library_title_1") == "Hotel California");
    assert(bindings.Text("library_title_2") == "Starboy");
    session.Action("library:track:2");
    assert(bindings.Text("track_title") == "Starboy" && bindings.Number("playback_progress") == 0);
    session.Action("nav:library");
    assert(bindings.Text("library_title_2") == "Bohemian Rhapsody");
    session.Action("library:track:2");
    assert(bindings.Text("track_title") == "Bohemian Rhapsody");
    assert(bindings.Number("playback_progress") == 0);
    session.Action("player:next");
    assert(bindings.Text("track_title") == "Starboy" && bindings.Number("playback_progress") == 0);
    session.Action("player:previous");
    assert(bindings.Text("track_title") == "Bohemian Rhapsody");
    assert(bindings.Number("playback_progress") == 0);
    session.Action("player:toggle");
    session.Tick(prism::sdk::MonotonicNs() + 600000000ULL);
    assert(bindings.Number("playback_progress") == 0 && bindings.Text("playback_icon") == "play");
    assert(session.TimeoutMs(prism::sdk::MonotonicNs(), -1) == -1);

    Bindings rejection;
    rejection.accept = false;
    prism::sdk::ModuleSession rejected(argv[1], "demo_player", 43,
                                       std::bind_front(&Bindings::Set, &rejection), {}, {}, {}, {},
                                       {}, {}, assets);
    assert(!rejected.Start() && !rejected.BackendReady() && !rejected.WorkPending());
    assert(!rejected.Start());
    rejected.Tick(prism::sdk::MonotonicNs() + 1000000000ULL);
    prism::sdk::ModuleSession wrong_identity(argv[1], "demo_player", 0, sink, {}, {}, {}, {}, {},
                                             {}, assets);
    assert(!wrong_identity.Start());
}
