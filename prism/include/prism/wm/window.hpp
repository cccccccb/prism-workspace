#pragma once

#include "prism/core/types.hpp"
#include "prism/scene/node.hpp"
#include "prism/lifecycle/window_state.hpp"
#include "prism/ipc/channel.hpp"
#include <memory>
#include <string>
#include <unordered_map>

namespace prism::render {
class FrameBuffer;
}

namespace prism::wm {

class Window {
public:
    Window(std::string app_id, std::string title, core::Rect bounds, std::shared_ptr<ipc::Channel> channel);
    ~Window();

    void SetPreviewTree(std::shared_ptr<scene::SceneNode> preview);
    void SetMasterTree(std::shared_ptr<scene::SceneNode> master);

    // Wayland Native Client Surface buffer attachment
    void AttachSurface(std::shared_ptr<render::FrameBuffer> surface);
    std::shared_ptr<render::FrameBuffer> GetSurface() const { return surface_buffer_; }

    void TransitionTo(std::unique_ptr<lifecycle::WindowState> new_state);

    void Tick(float dt);
    void OnMasterReady();

    void UpdateSlot(uint32_t slot_id, const std::string& val);
    void UpdateSlot(uint32_t slot_id, double val);

    void SetBounds(core::Rect bounds) { bounds_ = bounds; }

    void Render();

    // Accessors
    const std::string& GetAppId() const { return app_id_; }
    const std::string& GetTitle() const { return title_; }
    const core::Rect& GetBounds() const { return bounds_; }
    std::shared_ptr<ipc::Channel> GetChannel() const { return channel_; }
    lifecycle::WindowState* GetState() const { return current_state_.get(); }
    std::shared_ptr<scene::SceneNode> GetPreviewTree() const { return preview_tree_; }
    std::shared_ptr<scene::SceneNode> GetMasterTree() const { return master_tree_; }

    void DestroyPreviewTree() { preview_tree_.reset(); }

private:
    void IndexSlots(const std::shared_ptr<scene::SceneNode>& node);

    std::string app_id_;
    std::string title_;
    core::Rect bounds_;
    std::shared_ptr<ipc::Channel> channel_;
    std::unique_ptr<lifecycle::WindowState> current_state_;

    std::shared_ptr<scene::SceneNode> preview_tree_;
    std::shared_ptr<scene::SceneNode> master_tree_;
    std::unordered_map<uint32_t, std::shared_ptr<scene::SceneNode>> slot_index_;
    std::shared_ptr<render::FrameBuffer> surface_buffer_{nullptr};
};

} // namespace prism::wm
