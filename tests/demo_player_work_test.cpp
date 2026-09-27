#include "prism/runtime/task_scheduler.hpp"
#include "prism/sdk/module_session.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace std::chrono_literals;
using prism::runtime::PropertyValue;
using prism::runtime::TaskScheduler;
using prism::sdk::ModuleSession;

struct Track {
    std::string id, title, artist;
    unsigned duration_seconds;
    bool favorite;
};

struct Catalog {
    unsigned version{1};
    std::vector<Track> tracks;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Track, id, title, artist, duration_seconds, favorite)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Catalog, version, tracks)

class Assets {
public:
    Assets()
    {
        std::array<char, 64> path{};
        const std::string pattern = "/tmp/prism-player-work-XXXXXX";
        std::copy(pattern.begin(), pattern.end(), path.begin());
        const auto *created = mkdtemp(path.data());
        assert(created);
        root_ = created;
    }

    ~Assets()
    {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    const std::filesystem::path &Root() const
    {
        return root_;
    }

    void Write(const Catalog &catalog) const
    {
        std::ofstream file(root_ / "catalog.json", std::ios::binary);
        assert(file);
        file << nlohmann::json(catalog).dump();
        assert(file);
    }

    void WriteOversized() const
    {
        std::ofstream file(root_ / "catalog.json", std::ios::binary);
        assert(file);
        file << std::string(64 * 1024 + 1, 'x');
        assert(file);
    }

private:
    std::filesystem::path root_;
};

Catalog GoodCatalog()
{
    return {1,
            {{"one", "First \"quoted\" title", "Alpha", 125, false},
             {"two", "Second title", "Beta", 242, true}}};
}

struct Bindings {
    const std::thread::id owner{std::this_thread::get_id()};
    std::map<std::string, PropertyValue, std::less<>> values;
    std::size_t calls{};

    bool Set(std::string_view name, PropertyValue value)
    {
        assert(std::this_thread::get_id() == owner);
        values.insert_or_assign(std::string(name), std::move(value));
        ++calls;
        return true;
    }

    const std::string &Text(std::string_view name) const
    {
        return std::get<std::string>(values.at(std::string(name)));
    }

    bool Boolean(std::string_view name) const
    {
        return std::get<bool>(values.at(std::string(name)));
    }

    double Number(std::string_view name) const
    {
        return std::get<double>(values.at(std::string(name)));
    }
};

std::unique_ptr<ModuleSession> Session(const char *module, const Assets &assets, Bindings &bindings,
                                       std::shared_ptr<TaskScheduler> scheduler = {})
{
    return std::make_unique<ModuleSession>(
        module, "demo_player", 1, std::bind_front(&Bindings::Set, &bindings),
        ModuleSession::LaunchSink{}, ModuleSession::SubscribeSink{}, ModuleSession::ThemeSink{},
        ModuleSession::ColorSchemeSink{}, std::move(scheduler), prism::sdk::ModuleSessionLimits{},
        assets.Root());
}

void AwaitResult(ModuleSession &session)
{
    pollfd source{session.WorkCompletionFd(), POLLIN, 0};
    assert(source.fd >= 0 && poll(&source, 1, 5000) == 1 && source.revents & POLLIN);
}

void DeliverResult(ModuleSession &session)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (session.WorkPending()) {
        assert(std::chrono::steady_clock::now() < deadline);
        AwaitResult(session);
        assert(session.DispatchWork() > 0);
    }
}

struct EmptyOutput : prism::runtime::TaskOutput {
    std::uint64_t RetainedBytes() const noexcept override
    {
        return sizeof(*this);
    }
};

class WorkerBarrier {
public:
    std::shared_ptr<const prism::runtime::TaskOutput> Run(std::stop_token stop)
    {
        std::unique_lock lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        if (!changed_.wait(lock, stop, std::bind_front(&WorkerBarrier::Released, this))) {
            return {};
        }
        return std::make_shared<const EmptyOutput>();
    }

    void Await()
    {
        std::unique_lock lock(mutex_);
        assert(changed_.wait_for(lock, 5s, std::bind_front(&WorkerBarrier::Entered, this)));
    }

    void Release()
    {
        std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }

private:
    bool Entered() const
    {
        return entered_;
    }

    bool Released() const
    {
        return released_;
    }

    std::mutex mutex_;
    std::condition_variable_any changed_;
    bool entered_{}, released_{};
};

void PendingAndOwnerCompletion(const char *module)
{
    Assets assets;
    assets.Write(GoodCatalog());
    Bindings bindings;
    auto scheduler = std::make_shared<TaskScheduler>(nullptr, 1);
    WorkerBarrier barrier;
    auto channel = scheduler->OpenChannel();
    assert(channel->Submit({7, prism::runtime::TaskPriority::Critical, 64 * 1024,
                            std::bind_front(&WorkerBarrier::Run, &barrier)}) ==
           prism::runtime::TaskSubmitResult::Accepted);
    barrier.Await();

    auto session = Session(module, assets, bindings, scheduler);
    // Successful Start enforces the same cooperative entry budget as production.
    assert(session->Start() && !session->BackendReady() && session->WorkPending());
    assert(bindings.Boolean("catalog_pending") && !bindings.Boolean("catalog_ready") &&
           !bindings.Boolean("catalog_error"));
    assert(bindings.Text("catalog_status") == "Loading library");
    session->Action("nav:library");
    session->Action("player:toggle");
    session->Action("player:next");
    session->Action("library:track:2");
    assert(bindings.Text("playback_icon") == "play");
    assert(session->TimeoutMs(prism::sdk::MonotonicNs(), -1) == -1);

    barrier.Release();
    AwaitResult(*session);
    assert(!session->BackendReady() && bindings.Boolean("catalog_pending"));
    DeliverResult(*session);
    assert(session->BackendReady() && !bindings.Boolean("catalog_pending") &&
           bindings.Boolean("catalog_ready") && !bindings.Boolean("catalog_error"));
    assert(bindings.Text("catalog_status") == "Library ready");
    assert(bindings.Text("track_title") == "First \"quoted\" title");
    assert(bindings.Text("track_artist") == "Alpha");
    assert(bindings.Text("library_title_1") == "First \"quoted\" title");
    assert(bindings.Text("library_title_2") == "Second title");
    assert(bindings.Boolean("library_row_1") && bindings.Boolean("library_row_2") &&
           !bindings.Boolean("library_row_3"));

    session->Action("library:track:2");
    assert(bindings.Text("track_title") == "Second title" &&
           bindings.Text("playback_duration") == "4:02");
    session->Action("player:toggle");
    assert(bindings.Text("playback_icon") == "pause");
    assert(session->TimeoutMs(prism::sdk::MonotonicNs(), 10000) <= 500);
    const auto progress = bindings.Number("playback_progress");
    session->Tick(prism::sdk::MonotonicNs() + 1000000000ULL);
    assert(bindings.Number("playback_progress") > progress);
    session->Action("player:toggle");
    session->Tick(prism::sdk::MonotonicNs() + 1000000000ULL);
    assert(bindings.Text("playback_icon") == "play" &&
           session->TimeoutMs(prism::sdk::MonotonicNs(), -1) == -1);
    session->Action("player:next");
    assert(bindings.Text("track_title") == "First \"quoted\" title");
    session->Action("player:previous");
    assert(bindings.Text("track_title") == "Second title");
    channel->Stop();
}

enum class BadCatalog { Missing, Invalid, Oversized };

void ErrorAndRetry(const char *module, BadCatalog bad)
{
    Assets assets;
    if (bad == BadCatalog::Invalid) {
        auto invalid = GoodCatalog();
        invalid.tracks[1].id = invalid.tracks[0].id;
        assets.Write(invalid);
    } else if (bad == BadCatalog::Oversized) {
        assets.WriteOversized();
    }
    Bindings bindings;
    auto session = Session(module, assets, bindings);
    assert(session->Start() && !session->BackendReady());
    DeliverResult(*session);
    assert(!session->BackendReady() && bindings.Boolean("catalog_error") &&
           !bindings.Boolean("catalog_pending") && !bindings.Boolean("catalog_ready"));
    assert(bindings.Text("catalog_status").starts_with("Library error:") &&
           bindings.Text("catalog_status").size() > std::string("Library error:").size());
    session->Action("player:toggle");
    assert(bindings.Text("playback_icon") == "play" &&
           session->TimeoutMs(prism::sdk::MonotonicNs(), -1) == -1);

    assets.Write(GoodCatalog());
    session->Action("player:retry");
    assert(session->WorkPending() && bindings.Boolean("catalog_pending") &&
           !session->BackendReady());
    DeliverResult(*session);
    assert(session->BackendReady() && bindings.Boolean("catalog_ready") &&
           !bindings.Boolean("catalog_error"));
    assert(bindings.Text("track_title") == "First \"quoted\" title");
}

void CloseBeforeDelivery(const char *module)
{
    Assets assets;
    assets.Write(GoodCatalog());
    Bindings bindings;
    auto session = Session(module, assets, bindings);
    assert(session->Start() && !session->BackendReady());
    AwaitResult(*session);
    const auto before = bindings.values;
    const auto calls = bindings.calls;
    session->StopWork();
    assert(!session->WorkPending() && !session->BackendReady() && session->DispatchWork() == 0);
    session->Action("player:retry");
    session->Action("player:toggle");
    assert(bindings.values == before && bindings.calls == calls);
    session.reset();
    assert(bindings.values == before && bindings.calls == calls);
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2);
    PendingAndOwnerCompletion(argv[1]);
    ErrorAndRetry(argv[1], BadCatalog::Missing);
    ErrorAndRetry(argv[1], BadCatalog::Invalid);
    ErrorAndRetry(argv[1], BadCatalog::Oversized);
    CloseBeforeDelivery(argv[1]);
}
