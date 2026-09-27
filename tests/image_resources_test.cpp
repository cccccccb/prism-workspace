#include "prism/runtime/image_resources.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <thread>
#include <variant>

namespace {
std::vector<prism::runtime::ImageUpdate> AwaitCompletion(prism::runtime::ImageResources& resources) {
    pollfd descriptor{resources.CompletionFd(),POLLIN,0};
    assert(descriptor.fd>=0);
    assert(poll(&descriptor,1,2000)==1 && (descriptor.revents&POLLIN));
    auto updates=resources.Poll();
    assert(!updates.empty());
    descriptor.revents=0;
    assert(poll(&descriptor,1,0)==0); // Poll consumes the completion notification.
    return updates;
}
}

namespace {
std::optional<prism::contracts::DisplayList> BuildAndCommit(prism::runtime::Scene& scene) {
    auto list=scene.Build(prism::contracts::WindowId{1});
    scene.AcknowledgeComposite();
    return list;
}
}
int main() {
    const auto runtime_thread = std::this_thread::get_id();
    std::atomic<bool> decoded_on_worker{false};
    prism::runtime::ImageResources resources([&](const std::string& uri)
        -> std::optional<prism::runtime::DecodedImage> {
        decoded_on_worker = std::this_thread::get_id() != runtime_thread;
        if (uri == "bad") return std::nullopt;
        return prism::runtime::DecodedImage{2, 2, std::vector<std::uint8_t>(16, 255)};
    });
    auto id = resources.Request("image");
    assert(id && resources.Request("image") == id);
    auto blueprint = prism::runtime::ParseBlueprint(
        "VStack { Image(\"image\") }",
        [&](std::string_view uri) { return resources.Request(std::string(uri)); });
    auto shape = [](std::string_view, double) { return prism::runtime::ShapedText{}; };
    prism::runtime::Scene scene(std::move(blueprint), shape);
    assert(scene.SetViewport({100, 100}));
    auto first = BuildAndCommit(scene);
    assert(first);
    for (const auto& command : first->commands)
        assert(!std::holds_alternative<prism::contracts::DrawImage>(command));
    assert(!BuildAndCommit(scene));
    std::vector<prism::runtime::ImageUpdate> updates;
    updates=AwaitCompletion(resources);
    assert(updates.size() == 1 && updates.front().state == prism::runtime::ImageState::Ready);
    assert(decoded_on_worker);
    assert(resources.Get(id) && resources.DecodedBytes() == 16);
    assert(scene.ImageReady(id, updates.front().intrinsic_size));
    assert(prism::runtime::Has(scene.PendingDirty(), prism::runtime::Dirty::Layout));
    auto second = BuildAndCommit(scene);
    assert(second);
    bool drew = false;
    for (const auto& command : second->commands)
        if (auto image = std::get_if<prism::contracts::DrawImage>(&command)) {
            drew = image->image == id && image->destination.width == 2 && image->destination.height == 2;
        }
    assert(drew);
    prism::runtime::Scene wallpaper(prism::runtime::ParseBlueprint("Card { Image(\"image\") }",
        [&](std::string_view) { return id; }), shape);
    assert(wallpaper.SetViewport({300, 180}));
    assert(wallpaper.ImageReady(id, {2, 2}));
    auto wallpaper_list = BuildAndCommit(wallpaper);
    assert(wallpaper_list);
    const auto* fill = std::get_if<prism::contracts::DrawImage>(&wallpaper_list->commands.front());
    assert(fill && fill->destination.width == 300 && fill->destination.height == 180);
    auto bad = resources.Request("bad");
    updates.clear();
    updates=AwaitCompletion(resources);
    assert(updates.size() == 1 && updates.front().id == bad);
    assert(resources.State(bad) == prism::runtime::ImageState::Failed);

    prism::runtime::ImageResources limited([](const std::string&)
        -> std::optional<prism::runtime::DecodedImage> {
        return prism::runtime::DecodedImage{2, 2, std::vector<std::uint8_t>(16, 255)};
    }, 8);
    auto too_large = limited.Request("too-large-for-budget");
    updates.clear();
    updates=AwaitCompletion(limited);
    assert(updates.size() == 1 && updates.front().id == too_large);
    assert(limited.State(too_large) == prism::runtime::ImageState::Failed);
    assert(limited.DecodedBytes() == 0);

    // A completion produced before poll and one produced while poll waits both
    // remain observable. Consuming one notification cannot consume a future job.
    std::mutex mutex;
    std::condition_variable decoder_wake;
    unsigned entered=0,allowed=0;
    int completion_fd=-1;
    {
        prism::runtime::ImageResources controlled([&](const std::string& uri)
            -> std::optional<prism::runtime::DecodedImage> {
            std::unique_lock lock(mutex);
            const unsigned job=++entered;
            decoder_wake.notify_all();
            decoder_wake.wait(lock,[&]{return allowed>=job;});
            if(uri=="failure") return std::nullopt;
            return prism::runtime::DecodedImage{1,1,std::vector<std::uint8_t>(4,255)};
        });
        completion_fd=controlled.CompletionFd();
        assert(completion_fd>=0 && (fcntl(completion_fd,F_GETFD)&FD_CLOEXEC));
        assert(fcntl(completion_fd,F_GETFL)&O_NONBLOCK);
        auto wait_started=[&](unsigned job) {
            std::unique_lock lock(mutex);
            assert(decoder_wake.wait_for(lock,std::chrono::seconds(2),[&]{return entered>=job;}));
        };
        auto release=[&](unsigned job) {
            {std::lock_guard lock(mutex);allowed=job;}
            decoder_wake.notify_all();
        };
        const auto early=controlled.Request("before-poll");
        wait_started(1);release(1);
        updates=AwaitCompletion(controlled);
        assert(updates.size()==1 && updates[0].id==early);
        assert(controlled.Request("before-poll")==early);
        pollfd quiet{completion_fd,POLLIN,0};assert(poll(&quiet,1,0)==0);
        const auto failed=controlled.Request("failure");
        wait_started(2);
        std::thread producer([&]{release(2);});
        updates=AwaitCompletion(controlled);producer.join();
        assert(updates.size()==1 && updates[0].id==failed &&
            updates[0].state==prism::runtime::ImageState::Failed);
        const auto subsequent=controlled.Request("after-drain");
        wait_started(3);release(3);
        updates=AwaitCompletion(controlled);
        assert(updates.size()==1 && updates[0].id==subsequent);
        assert(controlled.Poll().empty());
    }
    errno=0;
    assert(fcntl(completion_fd,F_GETFD)==-1 && errno==EBADF);
}
