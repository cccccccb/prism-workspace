#include "prism/ipc/ipc_server.hpp"
#include "prism/ipc/wm_messages.hpp"
#include "prism/pack/package.hpp"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server-core.h>

namespace {
struct TemporaryDirectory {
    std::filesystem::path path;

    TemporaryDirectory()
    {
        char pattern[] = "/tmp/prism-json-boundary-XXXXXX";
        const char *directory = mkdtemp(pattern);
        assert(directory);
        path = directory;
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void VerifyLegacyManifest(const std::filesystem::path &root)
{
    const auto directory = root / "legacy";
    std::filesystem::create_directory(directory);
    const auto manifest_path = directory / "manifest.json";
    const auto package_path = root / "legacy.prismpkg";
    const std::string title = "A \"quote\" \\ slash ☃";
    const nlohmann::json manifest{
        {"app_id", "legacy_demo"}, {"metadata", {{"name", "Nested name must not override root"}}},
        {"name", title},           {"version", "2.0"},
        {"exec", "legacy_demo"},
    };
    std::ofstream(manifest_path) << manifest.dump();

    assert(prism::pack::PackageManager::PackDirectory(directory.string(), package_path.string(),
                                                      directory.string()));
    const auto package = prism::pack::PackageManager::Inspect(package_path.string());
    assert(package && package->app_id == "legacy_demo" && package->app_name == title);
    assert(package->version == "2.0" && package->exec_entry == "legacy_demo");

    // Invalid syntax and wrong field types must fail before producing an archive.
    std::filesystem::remove(package_path);
    std::ofstream(manifest_path) << "{ invalid JSON }";
    assert(!prism::pack::PackageManager::PackDirectory(directory.string(), package_path.string(),
                                                       directory.string()));
    assert(!std::filesystem::exists(package_path));

    std::ofstream(manifest_path) << nlohmann::json{{"app_id", 17}}.dump();
    assert(!prism::pack::PackageManager::PackDirectory(directory.string(), package_path.string(),
                                                       directory.string()));
    assert(!std::filesystem::exists(package_path));
}

void VerifyTreeMessage()
{
    prism::ipc::TreeNodeMessage node;
    node.id = 7;
    node.type = "view";
    node.focused = false;
    node.name = "Window \"quoted\" \\ ☃";
    node.native = false;
    node.instance = 0;
    node.fraction = prism::ipc::FractionMessage{1.0 / 3.0, 0};

    const auto json = nlohmann::json::parse(nlohmann::json(node).dump());
    const auto restored = json.get<prism::ipc::TreeNodeMessage>();
    assert(restored.name == node.name && restored.native == node.native);
    assert(restored.instance == node.instance && restored.fraction);
    // Tree JSON keeps the original six significant digits at the wire boundary.
    assert(restored.fraction->width == 0.333333);
    assert(!json.contains("app_id") && !json.contains("nodes"));
}

void VerifyWireNumbers()
{
    prism::ipc::StatusReply reply;
    reply.fps = prism::ipc::WireFixedValue(59.9F, 1);
    const auto json = nlohmann::json::parse(nlohmann::json(reply).dump());
    assert(json.at("fps").get<double>() == 59.9);

    const prism::ipc::OutputModeMessage mode{1024, 600, 59.9, false, true};
    const auto output = nlohmann::json::parse(nlohmann::json(mode).dump());
    assert(output.at("refresh_hz").get<double>() == 59.9);
    assert(output.at("preferred") == false && output.at("current") == true);

    // Preserve the old stream's nearest rounding at an exact decimal half.
    assert(prism::ipc::WireFixedValue(0.25, 1) == 0.2);
}

void VerifyUnknownCommand(const std::filesystem::path &root)
{
    auto loop = std::unique_ptr<wl_event_loop, decltype(&wl_event_loop_destroy)>(
        wl_event_loop_create(), wl_event_loop_destroy);
    assert(loop);
    prism::ipc::IpcServer server(loop.get());
    const auto path = (root / "ipc.sock").string();
    assert(server.Start(path));

    const int client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(client >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    assert(path.size() < sizeof(address.sun_path));
    std::copy(path.begin(), path.end(), address.sun_path);
    assert(connect(client, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0);
    const std::string command = "unknown\"command\\token";
    const std::string request = command + '\n';
    assert(send(client, request.data(), request.size(), MSG_NOSIGNAL) ==
           static_cast<ssize_t>(request.size()));

    for (int count = 0; count < 4; ++count) {
        assert(wl_event_loop_dispatch(loop.get(), 0) == 0);
    }

    pollfd ready{client, POLLIN, 0};
    assert(poll(&ready, 1, 1000) == 1);
    char buffer[1024];
    const auto count = recv(client, buffer, sizeof(buffer), 0);
    close(client);
    assert(count > 0);
    const auto response = nlohmann::json::parse(std::string_view(buffer, count));
    assert(response.at("status") == "error");
    assert(response.at("message") == "Unknown command: " + command);
}
} // namespace

int main()
{
    TemporaryDirectory directory;
    VerifyLegacyManifest(directory.path);
    VerifyTreeMessage();
    VerifyWireNumbers();
    VerifyUnknownCommand(directory.path);
}
