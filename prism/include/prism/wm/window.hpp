#pragma once

#include "prism/core/types.hpp"
#include "prism/ipc/channel.hpp"
#include "prism/wm/layer_type.hpp"
#include <memory>
#include <string>
#include <unordered_map>

namespace prism::decoration {
class TilingWindowDecorator;
}

namespace prism::wm {

class Window {
public:
    Window(std::string app_id, std::string title, core::Rect bounds, std::shared_ptr<ipc::Channel> channel);
    ~Window();

    void SetBounds(core::Rect bounds);
    void SetFocused(bool focused);
    bool IsFocused() const { return is_focused_; }

    void SetLayerType(LayerType type) { layer_type_ = type; }
    LayerType GetLayerType() const { return layer_type_; }

    void SetExclusiveMargin(float margin) { exclusive_margin_ = margin; }
    float GetExclusiveMargin() const { return exclusive_margin_; }

    void SetDecorator(std::unique_ptr<decoration::TilingWindowDecorator> decorator);
    decoration::TilingWindowDecorator* GetDecorator() const { return decorator_.get(); }


    // Accessors
    const std::string& GetAppId() const { return app_id_; }
    const std::string& GetTitle() const { return title_; }
    const core::Rect& GetBounds() const { return bounds_; }
    std::shared_ptr<ipc::Channel> GetChannel() const { return channel_; }

private:

    std::string app_id_;
    std::string title_;
    core::Rect bounds_;
    std::shared_ptr<ipc::Channel> channel_;

    LayerType layer_type_{LayerType::App};
    float exclusive_margin_{0.0f};

    bool is_focused_{false};
    std::unique_ptr<decoration::TilingWindowDecorator> decorator_{nullptr};
};

} // namespace prism::wm
