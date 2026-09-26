#include "prism/wm/compositor.hpp"
#include "prism/wm/layer_type.hpp"
#include "prism/wm/wlr_server.hpp"
#include "prism/ipc/ipc_server.hpp"
#include "prism/layout/fluid_split_strategy.hpp"
#include "prism/layout/mission_control_strategy.hpp"
#include "prism/core/logging.hpp"
#include <thread>
#include <chrono>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <atomic>
#include <csignal>
#include <fstream>

using namespace prism;

static std::atomic<bool> g_running{true};

static void SigIntHandler(int sig) {
    (void)sig;
    g_running.store(false);
}

int main(int argc, char* argv[]) {
#ifndef PRISM_HAS_SHELL_CLIENTS
    PRISM_LOG_ERROR("WM-MAIN", "Desktop requires the Skia GLES client build");
    return 1;
#endif
    signal(SIGINT, SigIntHandler);
    signal(SIGTERM, SigIntHandler);

    PRISM_LOG_INFO("WM-MAIN", "=================================================");
    PRISM_LOG_INFO("WM-MAIN", "     Project Prism Desktop Compositor (wlroots) ");
    PRISM_LOG_INFO("WM-MAIN", "=================================================");

    // 1. Initialize Compositor Engine
    auto wm = std::make_shared<wm::Compositor>();
    if (!wm->Initialize()) {
        return 1;
    }

    // 2. Set Mac Mission Control Overview Strategy wrapping Mac Fluid Split (Decorator Pattern)
    auto split = std::make_unique<layout::MacFluidSplitStrategy>(0.5f);
    auto mc = std::make_unique<layout::MissionControlStrategy>(std::move(split));
    wm->SetLayoutStrategy(std::move(mc));

    // 3. Initialize wlroots Wayland Server Engine (Sway architecture)
    wm::WlrServer server(wm);
    if (!server.Initialize("wayland-prism-0")) {
        PRISM_LOG_ERROR("WM-MAIN", "Failed to initialize Wayland compositor");
        return 1;
    } else {
        setenv("WAYLAND_DISPLAY", server.GetSocketName().c_str(), 1);
        // Write readiness probe file early: Wayland & IPC sockets are already listening!
        {
            std::ofstream ready_out("/tmp/prism.ready");
            ready_out << "READY " << getpid() << " " << server.GetSocketName() << "\n";
            ready_out.flush();
        }
        const char* xdg_run = getenv("XDG_RUNTIME_DIR");
        if (xdg_run) {
            std::ofstream xdg_ready(std::string(xdg_run) + "/prism.ready");
            xdg_ready << "READY " << getpid() << " " << server.GetSocketName() << "\n";
            xdg_ready.flush();
        }
        PRISM_LOG_INFO("WM-MAIN", "PrismWM readiness flag written to /tmp/prism.ready and XDG_RUNTIME_DIR");
        PRISM_LOG_INFO("WM-MAIN", "Exported WAYLAND_DISPLAY=%s to environment for child processes", server.GetSocketName().c_str());

        server.Start();
    }

    if (!server.StartShellClients()) {
        PRISM_LOG_ERROR("WM-MAIN", "Failed to start shell clients");
        return 1;
    }
    if (g_running.load()) {
        PRISM_LOG_INFO("WM-MAIN", "=========================================================");
        PRISM_LOG_INFO("WM-MAIN", "  PrismWM Compositor is now running persistently (Active)");
        PRISM_LOG_INFO("WM-MAIN", "  Wayland Display Socket: %s", server.GetSocketName().c_str());
        PRISM_LOG_INFO("WM-MAIN", "  IPC Control Socket:     %s", server.GetIpcServer() ? server.GetIpcServer()->GetSocketPath().c_str() : "N/A");
        PRISM_LOG_INFO("WM-MAIN", "  Frame Rate: Dynamic VSync-driven Native Pipeline    ");
        PRISM_LOG_INFO("WM-MAIN", "  Press Ctrl+C (SIGINT) to terminate Window Manager.     ");
        PRISM_LOG_INFO("WM-MAIN", "=========================================================");

        // Sway architecture: pure event-driven dispatching.
        // Rendering and animations are driven at the display's native refresh rate in HandleOutputFrame.
        while (g_running.load()) {
            server.RunEventLoopIteration(100);
        }
    }

    PRISM_LOG_INFO("WM-MAIN", "Received stop signal -> Shutting down Compositor...");
    unlink("/tmp/prism.ready");
    if (const char* xdg = getenv("XDG_RUNTIME_DIR")) {
        unlink((std::string(xdg) + "/prism.ready").c_str());
    }
    server.Stop();

    PRISM_LOG_INFO("WM-MAIN", "Multi-Window Compositor execution completed cleanly.");
    return 0;
}
