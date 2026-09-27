#include "prism/host/event_wait.hpp"
#include "prism/launch/error.hpp"
#include "prism/sdk/app_host.hpp"
#include "prism/sdk/module_session.hpp"
#include "worker.hpp"
#include <charconv>
#include <csignal>
#include <iostream>
#include <unistd.h>

namespace {
const char *Name(prism::contracts::LaunchMilestone value)
{
    using M = prism::contracts::LaunchMilestone;
    switch (value) {
    case M::RuntimeReady:
        return "RuntimeReady";
    case M::SurfaceConfigured:
        return "SurfaceConfigured";
    case M::FirstPresented:
        return "FirstPresented";
    case M::BackendReady:
        return "BackendReady";
    case M::Failed:
        return "Failed";
    default:
        return "Unknown";
    }
}

void LogHostEvent(const prism::contracts::LaunchEvent &event)
{
    std::cout << "host event=" << Name(event.milestone) << " request=" << event.request.value
              << " instance=" << event.instance.value << " pid=" << event.pid
              << " monotonic_ns=" << prism::sdk::MonotonicNs()
              << " error=" << static_cast<unsigned>(event.error) << " detail=" << event.detail
              << std::endl;
}

} // namespace

int main(int argc, char **argv)
{
    prism::sdk::HostConfig config;
    std::filesystem::path package, apps;
    int worker_fd = -1, parent_pid = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help") {
            std::cout << "Usage: prism-app-host --package DIRECTORY [--wayland SOCKET]\n"
                         "                      [--request-id ID] [--instance-id ID]\n";
            return 0;
        }
        if (i + 1 >= argc) {
            std::cerr << "Missing argument\n";
            return 2;
        }
        const std::string_view value(argv[++i]);
        if (arg == "--package") {
            package = value;
        } else if (arg == "--apps-root") {
            apps = value;
        } else if (arg == "--worker-fd" || arg == "--parent-pid") {
            int number{};
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), number);
            if (error != std::errc{} || end != value.data() + value.size() || number < 0) {
                return 2;
            }
            if (arg == "--worker-fd") {
                worker_fd = number;
            } else {
                parent_pid = number;
            }
        } else if (arg == "--wayland") {
            config.socket = value;
        } else if (arg == "--request-id" || arg == "--instance-id") {
            std::uint64_t id{};
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), id);
            if (error != std::errc{} || end != value.data() + value.size() || !id) {
                return 2;
            }
            if (arg == "--request-id") {
                config.request = {id};
            } else {
                config.instance = {id};
            }
        } else {
            std::cerr << "Unknown option: " << arg << '\n';
            return 2;
        }
    }
    std::unique_ptr<prism::host::SignalWake> signals;
    try {
        signals = std::make_unique<prism::host::SignalWake>();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    const auto &stopping = signals->Stopping();
    if (worker_fd >= 0) {
        if (apps.empty() || !package.empty()) {
            return 2;
        }
        try {
            return RunWorker(worker_fd, apps, config.socket, parent_pid, stopping, signals->Fd());
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    if (package.empty()) {
        std::cerr << "--package is required\n";
        return 2;
    }
    config.on_event = LogHostEvent;
    try {
        auto loaded = prism::launch::LoadPackage(package);
        prism::sdk::AppHost host(std::move(config));
        if (!host.PrepareFrontend() || !host.Bind(loaded)) {
            return 1;
        }
        while (!stopping) {
            pollfd signal{signals->Fd(), POLLIN, 0};
            if (!host.Pump(-1, std::span(&signal, 1))) {
                return host.IsCloseRequested() ? 0 : 1;
            }
            if (signal.revents) {
                signals->Consume();
            }
        }
        host.Close();
        return 0;
    } catch (const prism::launch::LaunchFailure &error) {
        std::cerr << "Package failure code=" << static_cast<unsigned>(error.Code()) << ": "
                  << error.what() << '\n';
        return 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
