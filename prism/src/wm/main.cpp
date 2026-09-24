#include "prism/wm/compositor.hpp"
#include "prism/wm/wlr_server.hpp"
#include "prism/ipc/ipc_server.hpp"
#include "prism/compiler/binary_loader.hpp"
#include "prism/layout/fluid_split_strategy.hpp"
#include "prism/layout/mission_control_strategy.hpp"
#include "prism/render/framebuffer.hpp"
#include "prism/render/canvas_renderer.hpp"
#include "prism/core/logging.hpp"
#include <thread>
#include <chrono>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <atomic>
#include <csignal>

using namespace prism;

static std::atomic<bool> g_running{true};

static void SigIntHandler(int sig) {
    (void)sig;
    g_running.store(false);
}

void ExportSnapshot(std::shared_ptr<wm::Compositor> wm, render::FrameBuffer& fb, const std::string& ppm_path) {
    bool prev = wm->GetDrawSoftwareCursor();
    wm->SetDrawSoftwareCursor(true);
    wm->RenderToFrameBuffer(fb);
    wm->SetDrawSoftwareCursor(prev);
    fb.SavePPM(ppm_path);
    PRISM_LOG_INFO("WM-RENDER", "Exported GPU/Software Canvas frame to: %s", ppm_path.c_str());
}

int main(int argc, char* argv[]) {
    bool auto_exit = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--auto-exit" || std::string(argv[i]) == "--test") {
            auto_exit = true;
        }
    }

    signal(SIGINT, SigIntHandler);
    signal(SIGTERM, SigIntHandler);

    PRISM_LOG_INFO("WM-MAIN", "=================================================");
    PRISM_LOG_INFO("WM-MAIN", "  Project PrismWM Compositor (wlroots 0.17 / Sway) ");
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
        PRISM_LOG_WARN("WM-MAIN", "wlroots server failed to bind hardware display; falling back to virtual compositor mode");
    } else {
        server.Start();
        setenv("WAYLAND_DISPLAY", server.GetSocketName().c_str(), 1);
        PRISM_LOG_INFO("WM-MAIN", "Exported WAYLAND_DISPLAY=%s to environment for child processes", server.GetSocketName().c_str());
    }

    // 4. Create Window A from .prismpkg: Prism Music Studio
    std::string channel_a = "/prism_demo_player";
    auto win_a = wm->CreateWindow("demo_player", "Prism Music Studio", core::Rect{0, 0, 960, 1080}, channel_a);
    if (win_a) {
        auto prev_a = compiler::BinarySceneLoader::LoadFromFile("demos/demo_player.prismpkg:preview.prismb");
        if (!prev_a) prev_a = compiler::BinarySceneLoader::LoadFromFile("demos/demo_player/preview.prismb");
        if (prev_a) win_a->SetPreviewTree(prev_a);

        auto mast_a = compiler::BinarySceneLoader::LoadFromFile("demos/demo_player.prismpkg:master.prismb");
        if (!mast_a) mast_a = compiler::BinarySceneLoader::LoadFromFile("demos/demo_player/master.prismb");
        if (mast_a) win_a->SetMasterTree(mast_a);
    }

    // 5. Create Window B from .prismpkg: Prism System Monitor & Preferences
    std::string channel_b = "/prism_demo_settings";
    auto win_b = wm->CreateWindow("demo_settings", "System Preferences", core::Rect{960, 0, 960, 1080}, channel_b);
    if (win_b) {
        auto prev_b = compiler::BinarySceneLoader::LoadFromFile("demos/demo_settings.prismpkg:preview.prismb");
        if (!prev_b) prev_b = compiler::BinarySceneLoader::LoadFromFile("demos/demo_settings/preview.prismb");
        if (prev_b) win_b->SetPreviewTree(prev_b);

        auto mast_b = compiler::BinarySceneLoader::LoadFromFile("demos/demo_settings.prismpkg:master.prismb");
        if (!mast_b) mast_b = compiler::BinarySceneLoader::LoadFromFile("demos/demo_settings/master.prismb");
        if (mast_b) win_b->SetMasterTree(mast_b);
    }

    // 6. Fast fork & launch application backends to connect to IPC
    pid_t pid_a = fork();
    if (pid_a == 0) {
        execl("./build/demos/demo_player", "demo_player", channel_a.c_str(), nullptr);
        _exit(1);
    }
    pid_t pid_b = fork();
    if (pid_b == 0) {
        execl("./build/demos/demo_settings", "demo_settings", channel_b.c_str(), nullptr);
        _exit(1);
    }

    render::FrameBuffer fb(1920, 1080);

    if (auto_exit) {
        PRISM_LOG_INFO("WM-MAIN", "Initial 0ms Preview frame for both windows:");
        wm->Render();
        ExportSnapshot(wm, fb, "snapshots/frame_00_preview.ppm");

        // Compositor Frame Execution Loop (Initial feature sequence & snapshot generation)
        for (int frame = 0; frame <= 16 && g_running.load(); ++frame) {
        std::this_thread::sleep_for(std::chrono::milliseconds(180));
        server.RunEventLoopIteration(10);
        wm->Tick(0.18f);

        if (frame == 5) {
            // Settled 50/50 Split View
            ExportSnapshot(wm, fb, "snapshots/frame_05_master_split50.ppm");
        } else if (frame == 8) {
            // Divider dragged to 65%
            ExportSnapshot(wm, fb, "snapshots/frame_08_divider_drag.ppm");
        } else if (frame == 11) {
            // Mission Control Overview Grid
            ExportSnapshot(wm, fb, "snapshots/frame_11_mission_control.ppm");
        } else if (frame == 15) {
            // Restored Split View after focus click
            ExportSnapshot(wm, fb, "snapshots/frame_15_master_settled.ppm");
        }

        // At frame 4: User drags split divider towards 65% via cursor motion
        if (frame == 4) {
            PRISM_LOG_INFO("WM-USER", ">>> Pointer Event: Dragging Mac Split Divider to 65%% <<<");
            wm->InjectPointerMotion(1248.0f, 540.0f);
            if (auto mc_strat = dynamic_cast<layout::MissionControlStrategy*>(wm->GetLayoutStrategy())) {
                if (auto split_strat = dynamic_cast<layout::MacFluidSplitStrategy*>(mc_strat->GetBaseStrategy())) {
                    split_strat->SetTargetRatio(0.65f);
                }
            }
        }

        // At frame 7: User clicks Dark Mode toggle in System Settings
        if (frame == 7) {
            PRISM_LOG_INFO("WM-USER", ">>> User clicked 'theme:toggle' in System Preferences! <<<");
            wm->DispatchAction("demo_settings", "theme:toggle");
        }

        // At frame 9: 3-Finger Swipe Up -> Trigger macOS Mission Control Overview!
        if (frame == 9) {
            PRISM_LOG_INFO("WM-USER", ">>> Trackpad Gesture: 3-Finger Swipe Up -> Triggering Mission Control! <<<");
            wm->InjectGesture(core::GestureType::Swipe3FingerUp);
            if (auto mc_strat = dynamic_cast<layout::MissionControlStrategy*>(wm->GetLayoutStrategy())) {
                mc_strat->SetOverview(true);
            }
        }

        // At frame 12: In Mission Control, user clicks Window B card (System Preferences) to focus
        if (frame == 12) {
            PRISM_LOG_INFO("WM-USER", ">>> Pointer Click on Window B card in Mission Control -> Focusing & Restoring! <<<");
            wm->SetFocusedWindowIndex(1);
            if (auto mc_strat = dynamic_cast<layout::MissionControlStrategy*>(wm->GetLayoutStrategy())) {
                mc_strat->SetOverview(false);
            }
        }

        // At frame 14: User clicks Play/Pause in Music Player
        if (frame == 14) {
            PRISM_LOG_INFO("WM-USER", ">>> User clicked 'player:toggle' in Music Studio! <<<");
            wm->DispatchAction("demo_player", "player:toggle");
        }
    }
    }

    if (!auto_exit && g_running.load()) {
        PRISM_LOG_INFO("WM-MAIN", "=========================================================");
        PRISM_LOG_INFO("WM-MAIN", "  PrismWM Compositor is now running persistently (Active)");
        PRISM_LOG_INFO("WM-MAIN", "  Wayland Display Socket: %s", server.GetSocketName().c_str());
        PRISM_LOG_INFO("WM-MAIN", "  IPC Control Socket:     %s", server.GetIpcServer() ? server.GetIpcServer()->GetSocketPath().c_str() : "N/A");
        PRISM_LOG_INFO("WM-MAIN", "  Frame Rate: Dynamic VSync-driven (Sway architecture)   ");
        PRISM_LOG_INFO("WM-MAIN", "  Press Ctrl+C (SIGINT) to terminate Window Manager.     ");
        PRISM_LOG_INFO("WM-MAIN", "=========================================================");

        // Sway architecture: pure event-driven dispatching.
        // Rendering and animations are driven at the display's native refresh rate in HandleOutputFrame.
        while (g_running.load()) {
            server.RunEventLoopIteration(100);
        }
    }

    PRISM_LOG_INFO("WM-MAIN", "Received stop signal -> Shutting down Compositor...");
    server.Stop();

    // Terminate child backends cleanly
    if (pid_a > 0) { kill(pid_a, SIGTERM); waitpid(pid_a, nullptr, 0); }
    if (pid_b > 0) { kill(pid_b, SIGTERM); waitpid(pid_b, nullptr, 0); }

    PRISM_LOG_INFO("WM-MAIN", "Multi-Window Compositor execution completed cleanly.");
    return 0;
}
