#include "prism/wm/compositor.hpp"
#include "prism/wm/layer_type.hpp"
#include "prism/wm/wlr_server.hpp"
#include "prism/ipc/ipc_server.hpp"
#include "prism/core/logging.hpp"
#include <thread>
#include <chrono>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <atomic>
#include <csignal>
#include <charconv>
#include <sys/prctl.h>

using namespace prism;

static std::atomic<bool> g_running{true};

static void SigIntHandler(int sig) {
    (void)sig;
    g_running.store(false);
}

int main(int argc, char* argv[]) {
    int control_fd=-1, parent_pid=0;
    for (int i=1;i<argc;++i) {
        std::string_view option(argv[i]);
        if (++i>=argc) return 2;
        std::string_view value(argv[i]); int number{};
        auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),number);
        if (error!=std::errc{} || end!=value.data()+value.size() || number<=0) return 2;
        if (option=="--control-fd") control_fd=number;
        else if (option=="--parent-pid") parent_pid=number; else return 2;
    }
    if (parent_pid) {
        if (prctl(PR_SET_PDEATHSIG,SIGTERM) || getppid()!=parent_pid) return 1;
    }
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

    // 3. Initialize wlroots Wayland Server Engine (Sway architecture)
    wm::WlrServer server(wm);
    if (!server.Initialize("wayland-prism-0")) {
        PRISM_LOG_ERROR("WM-MAIN", "Failed to initialize Wayland compositor");
        return 1;
    } else {
        setenv("WAYLAND_DISPLAY", server.GetSocketName().c_str(), 1);
        server.Start();
    }

    if (!server.IsRunning()) return 1;
    if (control_fd>=0) {
        try { server.AttachControl(control_fd,parent_pid); }
        catch (const std::exception& error) { PRISM_LOG_ERROR("WM-MAIN","%s",error.what()); return 1; }
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
        while (g_running.load() && server.ControlHealthy()) {
            server.RunEventLoopIteration(100);
        }
    }

    PRISM_LOG_INFO("WM-MAIN", "Received stop signal -> Shutting down Compositor...");
    server.Stop();

    PRISM_LOG_INFO("WM-MAIN", "Multi-Window Compositor execution completed cleanly.");
    return server.ControlHealthy() ? 0 : 1;
}
