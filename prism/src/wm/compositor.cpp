#include "prism/wm/compositor.hpp"
#include "prism/layout/fluid_split_strategy.hpp"
#include "prism/layout/mission_control_strategy.hpp"
#include "prism/render/framebuffer.hpp"
#include "prism/render/canvas_renderer.hpp"
#include "prism/core/logging.hpp"
#include <cmath>
#include <algorithm>

namespace prism::wm {

Compositor::Compositor() = default;

Compositor::~Compositor() = default;

bool Compositor::Initialize() {
    PRISM_LOG_INFO("WM", "Initializing PrismWM Compositor Engine (wlroots/Wayland Native)...");

    if (!decoration_spec_) {
        decoration_spec_ = decoration::TilingDecorationSpec::CreateDefault();
    }

    // Default to Mac Fluid Split Layout strategy (Strategy Pattern)
    SetLayoutStrategy(std::make_unique<layout::MacFluidSplitStrategy>(0.5f));

    running_ = true;
    return true;
}

void Compositor::OnPointerMotion(float x, float y, float dx, float dy) {
    cursor_x_ = x;
    cursor_y_ = y;

    if (is_dragging_divider_) {
        // Interactive divider drag: recalculate ratio dynamically
        float new_ratio = std::max(0.2f, std::min(0.8f, x / 1920.0f));
        if (auto mc = dynamic_cast<layout::MissionControlStrategy*>(layout_strategy_.get())) {
            if (auto split = dynamic_cast<layout::MacFluidSplitStrategy*>(mc->GetBaseStrategy())) {
                split->SetTargetRatio(new_ratio);
                PRISM_LOG_DEBUG("WM-INPUT", "Dragging split divider -> Target ratio: %.2f", new_ratio);
            }
        } else if (auto split = dynamic_cast<layout::MacFluidSplitStrategy*>(layout_strategy_.get())) {
            split->SetTargetRatio(new_ratio);
            PRISM_LOG_DEBUG("WM-INPUT", "Dragging split divider -> Target ratio: %.2f", new_ratio);
        }
    }
}

void Compositor::OnPointerButton(uint32_t button, bool pressed) {
    if (button == 1 || button == 272 /* BTN_LEFT */) {
        if (pressed) {
            // Check if clicking near the split divider
            layout::MacFluidSplitStrategy* split = nullptr;
            if (auto mc = dynamic_cast<layout::MissionControlStrategy*>(layout_strategy_.get())) {
                split = dynamic_cast<layout::MacFluidSplitStrategy*>(mc->GetBaseStrategy());
            } else {
                split = dynamic_cast<layout::MacFluidSplitStrategy*>(layout_strategy_.get());
            }
            if (split) {
                float divider_x = 1920.0f * split->GetCurrentRatio();
                if (std::abs(cursor_x_ - divider_x) < 30.0f) {
                    is_dragging_divider_ = true;
                    PRISM_LOG_INFO("WM-INPUT", "Engaged interactive Mac Split-screen divider grab!");
                }
            }
        } else {
            if (is_dragging_divider_) {
                is_dragging_divider_ = false;
                PRISM_LOG_INFO("WM-INPUT", "Released split-screen divider (Spring momentum settles)");
            }
        }
    }
}

void Compositor::OnGesture(core::GestureType gesture, float val) {
    if (gesture == core::GestureType::Swipe3FingerUp) {
        PRISM_LOG_INFO("WM-GESTURE", ">>> 3-Finger Swipe Up: Triggering macOS Mission Control Overview! <<<");
        ToggleMissionControl();
    } else if (gesture == core::GestureType::PinchZoom) {
        PRISM_LOG_INFO("WM-GESTURE", ">>> Pinch-to-zoom: Scale factor %.2f <<<", val);
    }
}

std::shared_ptr<Window> Compositor::CreateWindow(
    const std::string& app_id,
    const std::string& title,
    core::Rect bounds,
    const std::string& channel_name
) {
    auto channel = ipc::Channel::CreateHost(channel_name);
    if (!channel) {
        PRISM_LOG_ERROR("WM", "Failed to create IPC host for window: %s", app_id.c_str());
        return nullptr;
    }

    auto win = std::make_shared<Window>(app_id, title, bounds, std::move(channel));
    windows_.push_back(win);

    // Insert into Multi-Level Recursive BSP Tree Engine
    tree_engine_.InsertWindow(win, tree::Direction::Right);

    PRISM_LOG_INFO("WM", "Registered managed window '%s' on channel '%s' in TreeEngine", app_id.c_str(), channel_name.c_str());
    return win;
}

void Compositor::DestroyWindow(const std::shared_ptr<Window>& win) {
    if (!win) return;
    auto it = std::find(windows_.begin(), windows_.end(), win);
    if (it != windows_.end()) {
        windows_.erase(it);
    }
    tree_engine_.RemoveWindow(win);
    PRISM_LOG_INFO("WM", "Destroyed managed window '%s' from TreeEngine", win->GetAppId().c_str());
}

void Compositor::SetLayoutStrategy(std::unique_ptr<layout::LayoutStrategy> strategy) {
    if (strategy) {
        PRISM_LOG_INFO("WM", "Activated Layout Strategy: %s", strategy->GetStrategyName().c_str());
        layout_strategy_ = std::move(strategy);
    }
}

void Compositor::Tick(float dt) {
    // 1. Step layout animation physics & apply window geometry
    if (IsInMissionControl()) {
        if (layout_strategy_) {
            layout_strategy_->StepPhysics(dt);

            if (windows_.size() >= 2) {
                core::Rect screen{0, 30.0f, static_cast<float>(screen_width_), static_cast<float>(screen_height_ - 30)};
                core::Rect bounds_a, bounds_b;
                layout_strategy_->CalculateLayout(screen, bounds_a, bounds_b);
                windows_[0]->SetBounds(bounds_a);
                windows_[1]->SetBounds(bounds_b);
            }
        }
    } else {
        // Multi-Level Recursive BSP Tree Layout arrangement
        if (!decoration_spec_) {
            decoration_spec_ = decoration::TilingDecorationSpec::CreateDefault();
        }
        core::Rect screen{0.0f, 30.0f, static_cast<float>(screen_width_), static_cast<float>(screen_height_ - 30)};
        tree_engine_.Arrange(screen, *decoration_spec_);
        auto layout = tree_engine_.GetCalculatedLayout();
        for (const auto& [win, rect] : layout) {
            if (win) {
                win->SetBounds(rect);
            }
        }
    }

    // 2. Poll IPC updates and step each managed window
    for (auto& win : windows_) {
        auto channel = win->GetChannel();
        if (!channel) continue;

        ipc::StateDiffPacket diff;
        while (channel->PopStateDiff(diff)) {
            PRISM_LOG_INFO("WM-DIFF", "[%s] Popped diff op=%d slot=0x%08X", win->GetAppId().c_str(), static_cast<int>(diff.op), diff.slot_id);
            switch (diff.op) {
                case ipc::DiffOp::SignalReady:
                    PRISM_LOG_INFO("WM", "[%s] Received SignalReady from backend -> Triggering master morph", win->GetAppId().c_str());
                    win->OnMasterReady();
                    break;

                case ipc::DiffOp::SetString:
                    win->UpdateSlot(diff.slot_id, diff.value.str);
                    break;

                case ipc::DiffOp::SetInt64:
                    win->UpdateSlot(diff.slot_id, diff.value.i64);
                    break;

                case ipc::DiffOp::SetFloat:
                    win->UpdateSlot(diff.slot_id, diff.value.f64);
                    break;

                case ipc::DiffOp::SetBool:
                    win->UpdateSlot(diff.slot_id, diff.value.b);
                    break;

                case ipc::DiffOp::AppExit:
                    PRISM_LOG_INFO("WM", "[%s] Application requested exit", win->GetAppId().c_str());
                    break;

                default:
                    break;
            }
        }

        win->Tick(dt);
    }
}

void Compositor::Render() {
    for (auto& win : windows_) {
        win->Render();
    }
}

void Compositor::DispatchAction(const std::string& app_id, const std::string& action) {
    for (auto& win : windows_) {
        if (win->GetAppId() == app_id && win->GetChannel()) {
            auto ev = ipc::EventPacket::MakeAction(action);
            win->GetChannel()->PushEvent(ev);
            win->GetChannel()->NotifyPeer();
            PRISM_LOG_INFO("WM", "Dispatched UI Action '%s' to backend '%s'", action.c_str(), app_id.c_str());
            return;
        }
    }
    PRISM_LOG_WARN("WM", "No target window found for action: %s", action.c_str());
}

void Compositor::ToggleMissionControl() {
    if (auto mc = dynamic_cast<layout::MissionControlStrategy*>(layout_strategy_.get())) {
        mc->ToggleOverview();
    }
}

void Compositor::SetMissionControl(bool enabled) {
    if (auto mc = dynamic_cast<layout::MissionControlStrategy*>(layout_strategy_.get())) {
        mc->SetOverview(enabled);
    }
}

bool Compositor::IsInMissionControl() const {
    if (auto mc = dynamic_cast<layout::MissionControlStrategy*>(layout_strategy_.get())) {
        return mc->IsOverviewActive();
    }
    return false;
}

void Compositor::SetFocusedWindowIndex(int idx) {
    if (idx >= 0 && idx < static_cast<int>(windows_.size())) {
        focused_window_index_ = idx;
        for (size_t i = 0; i < windows_.size(); ++i) {
            windows_[i]->SetFocused(static_cast<int>(i) == idx);
        }
        tree_engine_.SetFocusedWindow(windows_[idx]);
        PRISM_LOG_INFO("WM-FOCUS", "Active focus shifted to window [%d: '%s']", idx, windows_[idx]->GetTitle().c_str());
    }
}

bool Compositor::MoveFocus(tree::Direction dir) {
    if (tree_engine_.MoveFocus(dir)) {
        auto win = tree_engine_.GetFocusedWindow();
        if (win) {
            for (size_t i = 0; i < windows_.size(); ++i) {
                if (windows_[i] == win) {
                    focused_window_index_ = static_cast<int>(i);
                    windows_[i]->SetFocused(true);
                } else {
                    windows_[i]->SetFocused(false);
                }
            }
        }
        return true;
    }
    return false;
}

bool Compositor::SwitchWorkspace(const std::string& name) {
    bool ok = tree_engine_.SwitchWorkspace(name);
    if (ok) {
        auto win = tree_engine_.GetFocusedWindow();
        if (win) {
            for (size_t i = 0; i < windows_.size(); ++i) {
                if (windows_[i] == win) {
                    focused_window_index_ = static_cast<int>(i);
                    windows_[i]->SetFocused(true);
                } else {
                    windows_[i]->SetFocused(false);
                }
            }
        }
    }
    return ok;
}

bool Compositor::SetTreeLayout(tree::LayoutMode mode) {
    auto focused = tree_engine_.GetFocusedNode();
    if (!focused) {
        auto ws = tree_engine_.GetActiveWorkspace();
        if (ws) focused = ws->GetRootContainer();
    }
    if (focused) {
        return tree_engine_.SetLayoutMode(focused, mode);
    }
    return false;
}

bool Compositor::SwapFocusDirection(tree::Direction dir) {
    return tree_engine_.SwapFocusDirection(dir);
}

void Compositor::RenderToFrameBuffer(render::FrameBuffer& fb) {
    // 1. macOS Dynamic Dark Nebula wallpaper (Cached static buffer matched to current output dimensions)
    static render::FrameBuffer s_wallpaper(0, 0);
    if (s_wallpaper.GetWidth() != fb.GetWidth() || s_wallpaper.GetHeight() != fb.GetHeight()) {
        s_wallpaper = render::FrameBuffer(fb.GetWidth(), fb.GetHeight());
        s_wallpaper.DrawDesktopGradient();
    }
    std::memcpy(fb.GetPixelsMutable(), s_wallpaper.GetPixels(), fb.GetWidth() * fb.GetHeight() * sizeof(uint32_t));

    // 2. Check layout mode (Split vs Mission Control Overview)
    float mc_progress = 0.0f;
    layout::MacFluidSplitStrategy* split_strat = nullptr;
    if (auto mc = dynamic_cast<layout::MissionControlStrategy*>(layout_strategy_.get())) {
        mc_progress = mc->GetProgress();
        split_strat = dynamic_cast<layout::MacFluidSplitStrategy*>(mc->GetBaseStrategy());
    } else {
        split_strat = dynamic_cast<layout::MacFluidSplitStrategy*>(layout_strategy_.get());
    }

    // 3. Render Windows (Wayland Client Surface Buffer or Preview/Master AST)
    for (size_t i = 0; i < windows_.size(); ++i) {
        const auto& win = windows_[i];
        if (win->GetSurface()) {
            auto b = win->GetBounds();
            fb.DrawShadow(static_cast<int>(b.x), static_cast<int>(b.y),
                          static_cast<int>(b.width), static_cast<int>(b.height),
                          16.0f, 24.0f, 0x99000000);
            fb.Blit(*win->GetSurface(),
                    static_cast<int>(b.x), static_cast<int>(b.y),
                    static_cast<int>(b.width), static_cast<int>(b.height));
        } else {
            auto tree = (win->GetState() && win->GetState()->GetStateName().find("Preview") != std::string::npos)
                        ? win->GetPreviewTree()
                        : win->GetMasterTree();
            if (tree) {
                render::CanvasRenderVisitor visitor(fb, win->GetBounds());
                tree->Accept(visitor);
            }
        }

        // In Mission Control mode: draw floating title badge above card
        if (mc_progress > 0.1f) {
            auto b = win->GetBounds();
            int badge_x = static_cast<int>(b.x + b.width * 0.5f - 80.0f);
            int badge_y = static_cast<int>(b.y - 28.0f);
            fb.DrawRoundedRect(badge_x, badge_y, 160, 24, 6.0f, 0x661e2028, mc_progress);
            fb.DrawTextSimple(badge_x + 12, badge_y + 8, win->GetTitle(), 0xFFFFFFFF);
        }
    }

    // 4. Split-screen divider handle (when not in Mission Control overview)
    if (split_strat && mc_progress < 0.2f && windows_.size() >= 2) {
        int divider_x = static_cast<int>(fb.GetWidth() * split_strat->GetCurrentRatio());
        fb.DrawSplitDivider(divider_x, 30, fb.GetHeight() - 30, is_dragging_divider_);
    }

    // 5. Mission Control Spaces bar overlay
    if (mc_progress > 0.05f) {
        fb.DrawMissionControlSpaces(mc_progress);
    }

    // 6. macOS Frosted Glass Top Menu Bar (height 30px)
    std::string active_title = "Prism Music Studio";
    if (focused_window_index_ == 1 && windows_.size() > 1) {
        active_title = "System Preferences";
    }
    fb.DrawTopMenuBar(active_title, "16:30");

    // 7. macOS Floating Glass Dock at bottom
    std::vector<std::string> dock_apps = {"Music", "Settings", "Terminal", "Files", "Browser"};
    fb.DrawMacDock(dock_apps, focused_window_index_);

    // 8. macOS Software Cursor (disabled by default in live wlroots compositor to prevent lag & double cursor; enabled for offline PPM snapshot generation)
    if (draw_software_cursor_) {
        fb.DrawCursor(static_cast<int>(cursor_x_), static_cast<int>(cursor_y_));
    }

    // 9. On-screen Debug Performance HUD (FPS, ms/frame, resolution, cursor coordinates)
    if (debug_hud_enabled_) {
        std::string mode_str = std::to_string(fb.GetWidth()) + "x" + std::to_string(fb.GetHeight()) + " (Native)";
        fb.DrawDebugHud(last_fps_, last_dt_ * 1000.0f, static_cast<int>(frame_count_), mode_str,
                        static_cast<int>(cursor_x_), static_cast<int>(cursor_y_));
    }
}

} // namespace prism::wm
