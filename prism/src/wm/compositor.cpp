#include "prism/wm/compositor.hpp"
#include "prism/decoration/tiling_window_decorator.hpp"
#include "prism/layout/fluid_split_strategy.hpp"
#include "prism/layout/mission_control_strategy.hpp"
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

    running_ = true;
    return true;
}

void Compositor::OnPointerMotion(float x, float y, float dx, float dy) {
    cursor_x_ = x;
    cursor_y_ = y;
    if (std::any_of(windows_.begin(), windows_.end(), [](const auto& win) { return win->IsNative(); })) return;

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
    if (std::any_of(windows_.begin(), windows_.end(), [](const auto& win) { return win->IsNative(); })) return;
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
    const std::string& channel_name,
    LayerType layer
) {
    // 1. Check singleton lease with LayerManager first
    if (layer != LayerType::App) {
        if (layer_manager_.IsLayerOccupied(layer)) {
            PRISM_LOG_ERROR("WM", "CreateWindow rejected: Layer '%s' already occupied! (Singleton violated for '%s')",
                            LayerTypeToString(layer), app_id.c_str());
            return nullptr;
        }
    }

    auto channel = ipc::Channel::CreateHost(channel_name);
    if (!channel) {
        PRISM_LOG_ERROR("WM", "Failed to create IPC host for window: %s", app_id.c_str());
        return nullptr;
    }

    auto win = std::make_shared<Window>(app_id, title, bounds, std::move(channel));

    // 2. Register to LayerManager
    auto res = layer_manager_.RegisterWindow(win, layer);
    if (res != LayerRegisterResult::Success) {
        PRISM_LOG_ERROR("WM", "Failed to register window '%s' on Layer '%s'", app_id.c_str(), LayerTypeToString(layer));
        return nullptr;
    }

    windows_.push_back(win);

    // 3. Setup Layer geometry
    if (layer == LayerType::Desktop) {
        win->SetBounds(core::Rect{0.0f, 0.0f, static_cast<float>(screen_width_), static_cast<float>(screen_height_)});
    } else if (layer == LayerType::TopBar) {
        float h = bounds.height > 0.0f ? bounds.height : 32.0f;
        win->SetBounds(core::Rect{0.0f, 0.0f, static_cast<float>(screen_width_), h});
        win->SetExclusiveMargin(h);
    } else if (layer == LayerType::Dock) {
        float h = bounds.height > 0.0f ? bounds.height : 68.0f;
        float w = bounds.width > 0.0f ? bounds.width : 600.0f;
        win->SetBounds(core::Rect{(screen_width_ - w) / 2.0f, screen_height_ - h - 10.0f, w, h});
        win->SetExclusiveMargin(h + 10.0f);
    } else if (layer == LayerType::App) {
        // Only standard App windows enter the BSP Tree Engine!
        tree_engine_.InsertWindow(win, tree::Direction::Right);
    }

    PRISM_LOG_INFO("WM", "Registered managed window '%s' on Layer '%s' in Compositor (channel '%s')",
                   app_id.c_str(), LayerTypeToString(layer), channel_name.c_str());
    return win;
}

void Compositor::DestroyWindow(const std::shared_ptr<Window>& win) {
    if (!win) return;
    auto it = std::find(windows_.begin(), windows_.end(), win);
    if (it != windows_.end()) {
        windows_.erase(it);
    }
    layer_manager_.UnregisterWindow(win);
    if (win->GetLayerType() == LayerType::App) {
        tree_engine_.RemoveWindow(win);
    }
    SynchronizeFocus();
    PRISM_LOG_INFO("WM", "Destroyed managed window '%s' on Layer '%s'",
                   win->GetAppId().c_str(), LayerTypeToString(win->GetLayerType()));
}

void Compositor::SetLayoutStrategy(std::unique_ptr<layout::LayoutStrategy> strategy) {
    if (strategy) {
        PRISM_LOG_INFO("WM", "Activated Layout Strategy: %s", strategy->GetStrategyName().c_str());
        layout_strategy_ = std::move(strategy);
    }
}

void Compositor::Tick(float dt) {
    // Native configure targets are arranged by WlrServer against the Shell
    // work area. Never apply the old model's independent fullscreen geometry.
    if (std::any_of(windows_.begin(), windows_.end(), [](const auto& win) { return win->IsNative(); })) return;
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
        // Multi-Level Recursive BSP Tree Layout arrangement inside dynamically negotiated safe area
        if (!decoration_spec_) {
            decoration_spec_ = decoration::TilingDecorationSpec::CreateDefault();
        }
        core::Rect usable_area = layer_manager_.CalculateUsableArea(screen_width_, screen_height_);
        tree_engine_.Arrange(usable_area, *decoration_spec_);
        auto layout = tree_engine_.GetCalculatedLayout();
        for (const auto& [win, rect] : layout) {
            if (win) {
                win->SetBounds(rect);
            }
        }
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
        SynchronizeFocus();
    }
    return ok;
}

bool Compositor::SetTreeLayout(tree::LayoutMode mode) {
    if (mode == tree::LayoutMode::SplitHorizontal || mode == tree::LayoutMode::SplitVertical) {
        return tree_engine_.SplitFocused(mode);
    }
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

std::shared_ptr<Window> Compositor::ManageNativeWindow(const std::string& app_id,
    const std::string& title, int pid, std::uint64_t instance) {
    auto win = std::make_shared<Window>(app_id, title, core::Rect{}, nullptr);
    win->SetNative(true);
    win->UpdateIdentity(app_id, title, pid, instance);
    windows_.push_back(win);
    if (layer_manager_.RegisterWindow(win, LayerType::App) != LayerRegisterResult::Success) {
        windows_.pop_back();
        return nullptr;
    }
    auto mode = tree_engine_.GetActiveWorkspace()->GetRootContainer()->GetLayoutMode();
    if (auto focused = tree_engine_.GetFocusedNode())
        if (auto parent = focused->GetParentContainer()) mode = parent->GetLayoutMode();
    tree_engine_.InsertWindow(win, mode == tree::LayoutMode::SplitVertical ? tree::Direction::Down : tree::Direction::Right);
    SynchronizeFocus();
    return win;
}

void Compositor::SynchronizeFocus() {
    auto focused = tree_engine_.GetFocusedWindow();
    focused_window_index_ = -1;
    for (std::size_t i = 0; i < windows_.size(); ++i) {
        windows_[i]->SetFocused(windows_[i] == focused);
        if (windows_[i] == focused) focused_window_index_ = static_cast<int>(i);
    }
}

} // namespace prism::wm
