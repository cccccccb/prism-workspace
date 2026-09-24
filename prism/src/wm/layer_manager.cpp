#include "prism/wm/layer_manager.hpp"
#include "prism/render/canvas_renderer.hpp"
#include "prism/decoration/tiling_window_decorator.hpp"
#include "prism/core/logging.hpp"
#include <algorithm>

namespace prism::wm {

LayerManager::LayerManager() {
    PRISM_LOG_INFO("WM-LAYER", "Initialized LayerManager with 4-Layer Hierarchy (Desktop, TopBar, Dock, AppGroup)");
}

LayerRegisterResult LayerManager::RegisterWindow(const std::shared_ptr<Window>& win, LayerType layer) {
    if (!win) return LayerRegisterResult::InvalidLayer;

    switch (layer) {
        case LayerType::Desktop:
            if (desktop_window_ && desktop_window_ != win) {
                PRISM_LOG_WARN("WM-LAYER", "Rejected duplicate Desktop client '%s'! Desktop layer is already occupied by '%s'",
                               win->GetAppId().c_str(), desktop_window_->GetAppId().c_str());
                return LayerRegisterResult::AlreadyExists;
            }
            desktop_window_ = win;
            win->SetLayerType(LayerType::Desktop);
            PRISM_LOG_INFO("WM-LAYER", "Registered Singleton [Layer 1: Desktop] -> '%s'", win->GetAppId().c_str());
            return LayerRegisterResult::Success;

        case LayerType::TopBar:
            if (topbar_window_ && topbar_window_ != win) {
                PRISM_LOG_WARN("WM-LAYER", "Rejected duplicate TopBar client '%s'! TopBar layer is already occupied by '%s'",
                               win->GetAppId().c_str(), topbar_window_->GetAppId().c_str());
                return LayerRegisterResult::AlreadyExists;
            }
            topbar_window_ = win;
            win->SetLayerType(LayerType::TopBar);
            if (win->GetExclusiveMargin() <= 0.0f) {
                win->SetExclusiveMargin(win->GetBounds().height > 0 ? win->GetBounds().height : 32.0f);
            }
            PRISM_LOG_INFO("WM-LAYER", "Registered Singleton [Layer 2: TopBar] -> '%s' (Exclusive Top Margin: %.1fpx)",
                           win->GetAppId().c_str(), win->GetExclusiveMargin());
            return LayerRegisterResult::Success;

        case LayerType::Dock:
            if (dock_window_ && dock_window_ != win) {
                PRISM_LOG_WARN("WM-LAYER", "Rejected duplicate Dock client '%s'! Dock layer is already occupied by '%s'",
                               win->GetAppId().c_str(), dock_window_->GetAppId().c_str());
                return LayerRegisterResult::AlreadyExists;
            }
            dock_window_ = win;
            win->SetLayerType(LayerType::Dock);
            if (win->GetExclusiveMargin() <= 0.0f) {
                win->SetExclusiveMargin(win->GetBounds().height > 0 ? win->GetBounds().height : 70.0f);
            }
            PRISM_LOG_INFO("WM-LAYER", "Registered Singleton [Layer 3: Dock] -> '%s' (Exclusive Bottom Margin: %.1fpx)",
                           win->GetAppId().c_str(), win->GetExclusiveMargin());
            return LayerRegisterResult::Success;

        case LayerType::AppGroup:
            if (has_app_group_ && app_group_window_ != win) {
                PRISM_LOG_WARN("WM-LAYER", "Rejected duplicate AppGroup Root client '%s'! AppGroup is already occupied by '%s'",
                               win->GetAppId().c_str(), app_group_window_ ? app_group_window_->GetAppId().c_str() : "Root");
                return LayerRegisterResult::AlreadyExists;
            }
            has_app_group_ = true;
            app_group_window_ = win;
            win->SetLayerType(LayerType::AppGroup);
            PRISM_LOG_INFO("WM-LAYER", "Registered Singleton [Layer 4: AppGroup Root] -> '%s'", win->GetAppId().c_str());
            return LayerRegisterResult::Success;

        case LayerType::App:
        default:
            win->SetLayerType(LayerType::App);
            return LayerRegisterResult::Success;
    }
}

void LayerManager::UnregisterWindow(const std::shared_ptr<Window>& win) {
    if (!win) return;

    if (desktop_window_ == win) {
        PRISM_LOG_INFO("WM-LAYER", "Released Singleton [Layer 1: Desktop] -> '%s'", win->GetAppId().c_str());
        desktop_window_.reset();
    } else if (topbar_window_ == win) {
        PRISM_LOG_INFO("WM-LAYER", "Released Singleton [Layer 2: TopBar] -> '%s'", win->GetAppId().c_str());
        topbar_window_.reset();
    } else if (dock_window_ == win) {
        PRISM_LOG_INFO("WM-LAYER", "Released Singleton [Layer 3: Dock] -> '%s'", win->GetAppId().c_str());
        dock_window_.reset();
    } else if (app_group_window_ == win) {
        PRISM_LOG_INFO("WM-LAYER", "Released Singleton [Layer 4: AppGroup] -> '%s'", win->GetAppId().c_str());
        app_group_window_.reset();
        has_app_group_ = false;
    }
}

bool LayerManager::IsLayerOccupied(LayerType layer) const {
    switch (layer) {
        case LayerType::Desktop:  return desktop_window_ != nullptr;
        case LayerType::TopBar:   return topbar_window_ != nullptr;
        case LayerType::Dock:     return dock_window_ != nullptr;
        case LayerType::AppGroup: return has_app_group_;
        case LayerType::App:
        default:                  return false;
    }
}

core::Rect LayerManager::CalculateUsableArea(int screen_w, int screen_h) const {
    float top_margin = 0.0f;
    if (topbar_window_) {
        top_margin = topbar_window_->GetExclusiveMargin() > 0.0f ?
                     topbar_window_->GetExclusiveMargin() :
                     topbar_window_->GetBounds().height;
    } else {
        top_margin = 30.0f; // Default built-in top menu bar fallback
    }

    float bottom_margin = 0.0f;
    if (dock_window_) {
        bottom_margin = dock_window_->GetExclusiveMargin() > 0.0f ?
                        dock_window_->GetExclusiveMargin() :
                        dock_window_->GetBounds().height;
    } else {
        bottom_margin = 70.0f; // Default built-in dock margin fallback
    }

    float usable_w = static_cast<float>(screen_w);
    float usable_h = std::max(100.0f, static_cast<float>(screen_h) - top_margin - bottom_margin);

    return core::Rect{0.0f, top_margin, usable_w, usable_h};
}

void LayerManager::RenderLayer(LayerType layer, render::FrameBuffer& fb) {
    std::shared_ptr<Window> win = nullptr;
    switch (layer) {
        case LayerType::Desktop:  win = desktop_window_; break;
        case LayerType::TopBar:   win = topbar_window_;  break;
        case LayerType::Dock:     win = dock_window_;    break;
        case LayerType::AppGroup: win = app_group_window_; break;
        default: break;
    }

    if (!win) return;

    auto b = win->GetBounds();

    // 1. Decorator rendering if window is decorated with a DSL theme
    if (win->GetDecorator() && win->GetDecorator()->GetSpec()) {
        const auto& spec = *win->GetDecorator()->GetSpec();
        if (spec.header.show_header) {
            auto hb = win->GetDecorator()->GetHeaderBounds();
            fb.DrawRoundedRect(hb.x, hb.y, hb.width, hb.height, 4.0f,
                               win->IsFocused() ? spec.header.bg_focused.ToHex() : spec.header.bg_unfocused.ToHex());
            fb.DrawTextSimple(hb.x + 8, hb.y + 6, win->GetTitle(),
                              win->IsFocused() ? spec.header.title_focused.ToHex() : spec.header.title_unfocused.ToHex());
        }
        if (spec.border.width > 0) {
            fb.DrawBorder(b.x, b.y, b.width, b.height, spec.border.corner_radius, spec.border.width,
                          win->IsFocused() ? spec.border.color_focused.ToHex() : spec.border.color_unfocused.ToHex());
        }
    }

    // 2. Client surface or DSL Scene Tree rendering
    if (win->GetSurface()) {
        if (layer != LayerType::Desktop) {
            fb.DrawShadow(static_cast<int>(b.x), static_cast<int>(b.y),
                          static_cast<int>(b.width), static_cast<int>(b.height),
                          16.0f, 20.0f, 0x88000000);
        }
        fb.Blit(*win->GetSurface(),
                static_cast<int>(b.x), static_cast<int>(b.y),
                static_cast<int>(b.width), static_cast<int>(b.height));
    } else {
        auto tree = (win->GetState() && win->GetState()->GetStateName().find("Preview") != std::string::npos)
                    ? win->GetPreviewTree()
                    : win->GetMasterTree();
        if (tree) {
            if (layer != LayerType::Desktop) {
                fb.DrawShadow(static_cast<int>(b.x), static_cast<int>(b.y),
                              static_cast<int>(b.width), static_cast<int>(b.height),
                              16.0f, 20.0f, 0x88000000);
            }
            render::CanvasRenderVisitor visitor(fb, b);
            tree->Accept(visitor);
        }
    }
}

} // namespace prism::wm
