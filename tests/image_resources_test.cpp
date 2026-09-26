#include "prism/runtime/image_resources.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>
#include <variant>

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
    auto first = scene.Build(prism::contracts::WindowId{1});
    assert(first);
    for (const auto& command : first->commands)
        assert(!std::holds_alternative<prism::contracts::DrawImage>(command));
    assert(!scene.Build(prism::contracts::WindowId{1}));
    std::vector<prism::runtime::ImageUpdate> updates;
    for (int i = 0; i < 100 && updates.empty(); ++i) {
        updates = resources.Poll();
        if (updates.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(updates.size() == 1 && updates.front().state == prism::runtime::ImageState::Ready);
    assert(decoded_on_worker);
    assert(resources.Get(id) && resources.DecodedBytes() == 16);
    assert(scene.ImageReady(id, updates.front().intrinsic_size));
    assert(prism::runtime::Has(scene.PendingDirty(), prism::runtime::Dirty::Layout));
    auto second = scene.Build(prism::contracts::WindowId{1});
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
    auto wallpaper_list = wallpaper.Build(prism::contracts::WindowId{1});
    assert(wallpaper_list);
    const auto* fill = std::get_if<prism::contracts::DrawImage>(&wallpaper_list->commands.front());
    assert(fill && fill->destination.width == 300 && fill->destination.height == 180);
    auto bad = resources.Request("bad");
    updates.clear();
    for (int i = 0; i < 100 && updates.empty(); ++i) {
        updates = resources.Poll();
        if (updates.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(updates.size() == 1 && updates.front().id == bad);
    assert(resources.State(bad) == prism::runtime::ImageState::Failed);

    prism::runtime::ImageResources limited([](const std::string&)
        -> std::optional<prism::runtime::DecodedImage> {
        return prism::runtime::DecodedImage{2, 2, std::vector<std::uint8_t>(16, 255)};
    }, 8);
    auto too_large = limited.Request("too-large-for-budget");
    updates.clear();
    for (int i = 0; i < 100 && updates.empty(); ++i) {
        updates = limited.Poll();
        if (updates.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(updates.size() == 1 && updates.front().id == too_large);
    assert(limited.State(too_large) == prism::runtime::ImageState::Failed);
    assert(limited.DecodedBytes() == 0);
}
