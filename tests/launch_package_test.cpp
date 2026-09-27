#include "prism/launch/error.hpp"
#include "prism/launch/package.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace prism::launch;
const std::string manifest = R"({
"format_version":1,"runtime_abi":1,"app_id":"org.prism.music",
"name":"音乐","version":"1.0.0","ui":"main.prism",
"preview":"preview.prism","module":"backend.so","assets":"assets",
"window":{"width":960,"height":640}})";

void Reject(const std::function<void()> &task)
{
    bool rejected = false;
    try {
        task();
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);
}

std::string Replace(std::string text, const std::string &from, const std::string &to)
{
    text.replace(text.find(from), from.size(), to);
    return text;
}

int main()
{
    const auto info = ParseManifest(manifest);
    assert(info.app_id == "org.prism.music" && info.name == "音乐" && info.width == 960);
    for (const auto &pair : {std::pair{"\"runtime_abi\":1", "\"runtime_abi\":2"},
                             {"\"format_version\":1", "\"format_version\":1.0"},
                             {"\"width\":960", "\"width\":\"960\""},
                             {"\"width\":960", "\"width\":4097"},
                             {"main.prism", "../main.prism"},
                             {"main.prism", "/main.prism"},
                             {"org.prism.music", "../music"},
                             {"\"assets\":\"assets\"", "\"assets\":\"assets\",\"exec\":\"sh\""},
                             {"\"name\":\"音乐\"", "\"name\":\"音乐\",\"name\":\"other\""},
                             {"\"height\":640", "\"height\":640,\"height\":320"},
                             {"\"name\":\"音乐\"", "\"name\":\"\\u0000\""}}) {
        Reject([&] { ParseManifest(Replace(manifest, pair.first, pair.second)); });
    }
    try {
        ParseManifest(Replace(manifest, "\"runtime_abi\":1", "\"runtime_abi\":2"));
        assert(false);
    } catch (const LaunchFailure &error) {
        assert(error.Code() == prism::contracts::LaunchError::UnsupportedAbi);
    }
    try {
        ParseManifest("{}");
        assert(false);
    } catch (const LaunchFailure &error) {
        assert(error.Code() == prism::contracts::LaunchError::InvalidPackage);
    }
    Reject([] { ParseManifest("{}"); });
    Reject([] { ParseManifest(std::string(65537, ' ')); });
    Reject([] { ParseManifest("{not-json}"); });
    Reject([&] { ParseManifest(manifest + " false"); });
    assert(!ValidAppId("a;exec") && !ValidAppId(std::string(129, 'a')));
    char path[] = "/tmp/prism-package-test-XXXXXX";
    const auto *created = mkdtemp(path);
    assert(created);
    const fs::path root = created;
    try {
        fs::create_directory(root / "app");
        fs::create_directory(root / "app/assets");
        std::ofstream(root / "app/manifest.json") << manifest;
        for (auto name : {"main.prism", "preview.prism", "backend.so"}) {
            std::ofstream(root / "app" / name) << "fixture";
        }
        const auto package = LoadPackage(root / "app");
        assert(package.root == fs::canonical(root / "app") && package.preview);
        fs::create_directory(root / "registry");
        fs::copy(root / "app", root / "registry/org.prism.music", fs::copy_options::recursive);
        assert(LoadRegisteredPackage(root / "registry", "org.prism.music").manifest.app_id ==
               "org.prism.music");
        Reject([&] { LoadRegisteredPackage(root / "registry", "../app"); });
        try {
            LoadRegisteredPackage(root / "registry", "missing");
            assert(false);
        } catch (const LaunchFailure &error) {
            assert(error.Code() == prism::contracts::LaunchError::UnknownApplication);
        }
        fs::create_directory_symlink(root / "app", root / "registry/escape");
        Reject([&] { LoadRegisteredPackage(root / "registry", "escape"); });
        fs::create_directory_symlink(root / "registry/org.prism.music", root / "registry/mismatch");
        Reject([&] { LoadRegisteredPackage(root / "registry", "mismatch"); });
        fs::remove(root / "app/main.prism");
        std::ofstream(root / "outside.prism") << "outside";
        fs::create_symlink(root / "outside.prism", root / "app/main.prism");
        Reject([&] { LoadPackage(root / "app"); });
        fs::remove(root / "app/main.prism");
        Reject([&] { LoadPackage(root / "app"); });
        fs::create_directory(root / "app/main.prism");
        Reject([&] { LoadPackage(root / "app"); });
    } catch (...) {
        fs::remove_all(root);
        throw;
    }
    fs::remove_all(root);
}
