#include "prism/invoker/launch_pipeline.hpp"
#include "prism/launcher/zygote_server.hpp"
#include "prism/core/logging.hpp"
#include "prism/core/types.hpp"
#include <unistd.h>
#include <iostream>
#include <filesystem>
#include <thread>
#include <chrono>

using namespace prism;

bool WaitForWmReady(int timeout_ms = 5000) {
    auto t0 = std::chrono::steady_clock::now();
    std::string runtime_dir = getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "/tmp";
    while (true) {
        if (std::filesystem::exists("/tmp/prism.ready") ||
            std::filesystem::exists(runtime_dir + "/prism.ready") ||
            std::filesystem::exists(runtime_dir + "/prism-ipc.sock") ||
            std::filesystem::exists(runtime_dir + "/wayland-prism-0") ||
            std::filesystem::exists("/tmp/wayland-prism-0")) {
            return true;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (elapsed > timeout_ms) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

bool LaunchApp(const std::string& app_name, const std::string& channel_name, const std::string& pkg = "") {
    invoker::LaunchContext ctx;
    ctx.app_name = app_name;
    ctx.channel_name = channel_name.empty() ? ("/prism_" + app_name + "_" + std::to_string(getpid())) : channel_name;
    ctx.package_path = pkg;
    ctx.zygote_socket = launcher::DEFAULT_ZYGOTE_SOCKET;

    auto manifest_step = std::make_shared<invoker::ManifestValidationStep>();
    auto sandbox_step  = std::make_shared<invoker::SandboxSecurityStep>();
    auto preview_step  = std::make_shared<invoker::PreviewMountStep>();
    auto zygote_step   = std::make_shared<invoker::ZygoteDispatchStep>();

    manifest_step->SetNext(sandbox_step)
                 ->SetNext(preview_step)
                 ->SetNext(zygote_step);

    auto t_start = core::CurrentTimeNs();
    bool success = manifest_step->Handle(ctx);
    auto elapsed_us = (core::CurrentTimeNs() - t_start) / 1000.0;

    if (success) {
        PRISM_LOG_INFO("INVOKER", "Successfully invoked '%s' in %.2f us (%.3f ms)",
                       app_name.c_str(), elapsed_us, elapsed_us / 1000.0);
    } else {
        PRISM_LOG_ERROR("INVOKER", "Failed to invoke '%s'", app_name.c_str());
    }
    return success;
}

int main(int argc, char* argv[]) {
    PRISM_LOG_INFO("INVOKER-MAIN", "=================================================");
    PRISM_LOG_INFO("INVOKER-MAIN", "      Project Prism App & Shell Invoker          ");
    PRISM_LOG_INFO("INVOKER-MAIN", "=================================================");

    if (argc < 2) {
        std::cout << "Usage: prism-invoker [--shell | <app_name | app.prismpkg> [channel]]\n";
        std::cout << "       prism-invoker --wait-ready [timeout_ms]\n";
        return 0;
    }

    std::string arg1 = argv[1];

    if (arg1 == "--wait-ready") {
        int timeout = (argc > 2) ? std::stoi(argv[2]) : 5000;
        PRISM_LOG_INFO("INVOKER", "Waiting for PrismWM Compositor readiness (timeout: %d ms)...", timeout);
        if (WaitForWmReady(timeout)) {
            PRISM_LOG_INFO("INVOKER", "PrismWM Compositor is READY!");
            return 0;
        } else {
            PRISM_LOG_ERROR("INVOKER", "Timed out waiting for PrismWM Compositor!");
            return 1;
        }
    }

    if (arg1 == "--shell" || arg1 == "shell") {
        PRISM_LOG_INFO("INVOKER", "Bootstrapping System Shell Suite (Desktop, Dock, TopBar)...");
        if (!WaitForWmReady(5000)) {
            PRISM_LOG_WARN("INVOKER", "WM readiness flag not found within 5s, proceeding with launch anyway...");
        }

        bool ok_desk = LaunchApp("prism-desktop", "/prism_desktop_ipc");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        bool ok_dock = LaunchApp("prism-dock", "/prism_dock_ipc");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        bool ok_top = LaunchApp("prism-topbar", "/prism_topbar_ipc");

        if (ok_desk && ok_dock && ok_top) {
            PRISM_LOG_INFO("INVOKER", "All 3 Shell Components dispatched successfully!");
            return 0;
        }
        return 1;
    }

    std::string channel = (argc > 2) ? argv[2] : "";
    std::string pkg = (argc > 3) ? argv[3] : "";

    bool ok = LaunchApp(arg1, channel, pkg);
    return ok ? 0 : 1;
}
