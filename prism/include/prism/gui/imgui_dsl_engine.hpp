#pragma once

#include "prism/compiler/ast.hpp"
#include "prism/render/framebuffer.hpp"
#include <memory>
#include <functional>
#include <string>
#include <unordered_map>

struct ImGuiContext;

namespace prism::gui {

using ActionCallback = std::function<void(const std::string& action)>;

/**
 * @brief High-performance Declarative-to-Immediate DSL Engine.
 *        Binds Prism AST nodes and reactive slots directly to Dear ImGui
 *        widgets and smooth vector anti-aliased typography.
 */
class ImGuiDslEngine {
public:
    ImGuiDslEngine();
    ~ImGuiDslEngine();

    bool Initialize(int width, int height);
    void Shutdown();

    // Load TrueType vector font for ultra-crisp typography
    bool LoadFont(const std::string& font_path, float font_size_px = 15.0f);

    // Reactive Slot State Store
    void SetState(const std::string& key, const std::string& value);
    std::string GetState(const std::string& key, const std::string& default_val = "") const;

    // Action dispatch callback
    void SetActionCallback(ActionCallback cb) { action_callback_ = std::move(cb); }

    // Input feed (mouse coordinates and button state)
    void UpdateMouse(float mouse_x, float mouse_y, bool mouse_down);

    // Render an entire AST hierarchy into target FrameBuffer
    void RenderTree(const std::shared_ptr<compiler::AstNode>& root,
                    render::FrameBuffer& target_fb,
                    float dt = 0.016f);

    bool IsInitialized() const { return initialized_; }
    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }

    void SetActiveAppIndex(int idx) { active_app_index_ = idx; }
    int GetActiveAppIndex() const { return active_app_index_; }

private:
    std::string ResolveValue(const std::string& fallback, const std::string& slot) const;
    void RenderNode(const std::shared_ptr<compiler::AstNode>& node, float dt);

    int width_{0};
    int height_{0};
    bool initialized_{false};
    ImGuiContext* ctx_{nullptr};

    std::unordered_map<std::string, std::string> state_store_;
    ActionCallback action_callback_;

    bool in_dock_context_{false};
    bool in_topbar_context_{false};
    int topbar_spacer_count_{0};
    int dock_button_index_{0};
    int active_app_index_{0};
};

} // namespace prism::gui
