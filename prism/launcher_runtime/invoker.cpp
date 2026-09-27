#include "prism/sdk/launch_client.hpp"
#include <chrono>
#include <iostream>

int main(int argc, char **argv)
{
    std::string app, socket;
    auto mode = prism::contracts::LaunchMode::ActivateOrCreate;
    bool wait_exit = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--help") {
            std::cout << "Usage: prism-invoker APP_ID [--new] [--socket PATH] [--wait-exit]\n";
            return 0;
        }
        if (arg == "--new") {
            mode = prism::contracts::LaunchMode::NewInstance;
        } else if (arg == "--wait-exit") {
            wait_exit = true;
        } else if (arg == "--socket" && i + 1 < argc) {
            socket = argv[++i];
        } else if (app.empty() && !arg.starts_with('-')) {
            app = arg;
        } else {
            return 2;
        }
    }
    if (app.empty()) {
        return 2;
    }
    try {
        prism::sdk::LaunchClient client(socket);
        const auto request = client.Launch(app, mode);
        if (!request) {
            std::cerr << "Launch service unavailable/request rejected locally\n";
            return 1;
        }
        bool ready = false, presented = false, failed = false, activated = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(65);
        while (client.Connected()) {
            for (const auto &event : client.Pump(100)) {
                std::cout << "request=" << event.request.value
                          << " instance=" << event.instance.value << " pid=" << event.pid
                          << " milestone=" << static_cast<unsigned>(event.milestone)
                          << " error=" << static_cast<unsigned>(event.error)
                          << " exit=" << event.exit_code << " detail=" << event.detail << std::endl;
                if (event.milestone == prism::contracts::LaunchMilestone::Failed) {
                    failed = true;
                }
                if (event.milestone == prism::contracts::LaunchMilestone::BackendReady) {
                    ready = true;
                }
                if (event.milestone == prism::contracts::LaunchMilestone::FirstPresented) {
                    presented = true;
                }
                if (event.milestone == prism::contracts::LaunchMilestone::Activated) {
                    activated = true;
                }
                if (event.milestone == prism::contracts::LaunchMilestone::Exited) {
                    return failed || event.exit_code != 0 ? 1 : 0;
                }
                if (failed && !event.pid) {
                    return 1;
                }
            }
            if ((activated || (ready && presented)) && !wait_exit) {
                return 0;
            }
            if (std::chrono::steady_clock::now() >= deadline &&
                !(activated || (ready && presented))) {
                client.Cancel({request});
                std::cerr << "Invoker startup timeout\n";
                return 1;
            }
        }
        std::cerr << "Launch service disconnected\n";
        return 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
