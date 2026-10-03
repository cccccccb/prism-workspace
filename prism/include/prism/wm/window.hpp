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
    Window(std::string app_id, std::string title, core::Rect bounds,
           std::shared_ptr<ipc::Channel> channel);
    ~Window();

    void SetBounds(core::Rect bounds);
    void SetFocused(bool focused);

    // Committed client constraints in logical coordinates; zero means unspecified.
    bool SetMinimumSize(float width, float height);

    float GetMinimumWidth() const
    {
        return minimum_width_;
    }

    float GetMinimumHeight() const
    {
        return minimum_height_;
    }

    bool IsFocused() const
    {
        return is_focused_;
    }

    void SetLayerType(LayerType type)
    {
        layer_type_ = type;
    }

    LayerType GetLayerType() const
    {
        return layer_type_;
    }

    void SetExclusiveMargin(float margin)
    {
        exclusive_margin_ = margin;
    }

    float GetExclusiveMargin() const
    {
        return exclusive_margin_;
    }

    void SetDecorator(std::unique_ptr<decoration::TilingWindowDecorator> decorator);

    decoration::TilingWindowDecorator *GetDecorator() const
    {
        return decorator_.get();
    }

    // Accessors
    const std::string &GetAppId() const
    {
        return app_id_;
    }

    const std::string &GetTitle() const
    {
        return title_;
    }

    const core::Rect &GetBounds() const
    {
        return bounds_;
    }

    std::shared_ptr<ipc::Channel> GetChannel() const
    {
        return channel_;
    }

    void UpdateIdentity(std::string app_id, std::string title, int pid, std::uint64_t instance)
    {
        app_id_ = std::move(app_id);
        title_ = std::move(title);
        pid_ = pid;
        instance_ = instance;
    }

    int GetPid() const
    {
        return pid_;
    }

    std::uint64_t GetInstance() const
    {
        return instance_;
    }

    void SetNative(bool value)
    {
        native_ = value;
    }

    bool IsNative() const
    {
        return native_;
    }

    void SetCommittedBounds(core::Rect value)
    {
        committed_bounds_ = value;
    }

    const core::Rect &GetCommittedBounds() const
    {
        return committed_bounds_;
    }

    void SetVisible(bool value)
    {
        visible_ = value;
    }

    bool IsVisible() const
    {
        return visible_;
    }

    void SetFullscreen(bool value)
    {
        fullscreen_ = value;
    }

    bool IsFullscreen() const
    {
        return fullscreen_;
    }

private:
    std::string app_id_;
    std::string title_;
    core::Rect bounds_;
    std::shared_ptr<ipc::Channel> channel_;
    core::Rect committed_bounds_{};
    float minimum_width_{0};
    float minimum_height_{0};
    int pid_{};
    std::uint64_t instance_{};
    bool native_{}, visible_{true}, fullscreen_{};

    LayerType layer_type_{LayerType::App};
    float exclusive_margin_{0.0f};

    bool is_focused_{false};
    std::unique_ptr<decoration::TilingWindowDecorator> decorator_{nullptr};
};

} // namespace prism::wm
