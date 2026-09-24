#pragma once

#include "prism/ipc/channel.hpp"
#include "prism/sdk/event_dispatcher.hpp"
#include "prism/core/noncopyable.hpp"
#include "prism/scene/node.hpp"
#include "prism/render/framebuffer.hpp"
#include <string>
#include <memory>
#include <unordered_map>

struct wl_display;

namespace prism::sdk {

struct AppConfig {
    std::string app_id;
    std::string package_path;
    std::string wayland_display;
    std::string channel_name;
    int width{960};
    int height{1080};
};

/**
 * @brief Facade Pattern: Unified high-level API for Wayland-native Prism applications.
 *        The client application parses the declarative DSL (.prismb), creates a Wayland
 *        client surface, renders UI widgets, manages reactive slots, and handles input events.
 */
class Application : public core::NonCopyable {
public:
    static std::shared_ptr<Application> Create(const AppConfig& config);
    static std::shared_ptr<Application> Connect(const std::string& channel_name = "");
    ~Application();

    // Event binding from Frontend UI (Observer Pattern)
    void On(const std::string& action, ActionHandler handler);

    // State Diff updates to Frontend UI ($slot bindings)
    void SetState(std::string_view slot_name, std::string_view str);
    void SetState(std::string_view slot_name, const char* str);
    void SetState(std::string_view slot_name, const std::string& str);
    void SetState(std::string_view slot_name, int64_t val);
    void SetState(std::string_view slot_name, double val);
    void SetState(std::string_view slot_name, bool val);

    // Signals WM that master tree is ready (Preview -> Master transition)
    void Ready();

    // Event Loop
    int Exec();
    void Exit(int code = 0);
    void PollEvents();

    // Hot-reload DSL without dropping backend application state
    bool HotReload(const std::string& package_path = "");

    // Client-side UI Rendering & Surface access
    void RenderSurface();
    std::shared_ptr<render::FrameBuffer> GetSurface() const { return client_surface_; }
    std::shared_ptr<scene::SceneNode> GetMasterTree() const { return master_tree_; }
    std::shared_ptr<scene::SceneNode> GetPreviewTree() const { return preview_tree_; }

    const std::string& GetAppId() const { return config_.app_id; }
    bool IsConnectedToWayland() const { return wl_display_ != nullptr; }

private:
    explicit Application(AppConfig config, std::shared_ptr<ipc::Channel> channel);

    void InitializeWaylandClient();
    void LoadPackageDsl();
    void IndexSlots(const std::shared_ptr<scene::SceneNode>& node);

    AppConfig config_;
    std::shared_ptr<ipc::Channel> channel_;
    EventDispatcher dispatcher_;
    bool running_{false};
    int exit_code_{0};

    // Client-side DSL scene trees
    std::shared_ptr<scene::SceneNode> preview_tree_;
    std::shared_ptr<scene::SceneNode> master_tree_;
    std::unordered_map<uint32_t, std::shared_ptr<scene::SceneNode>> slot_index_;
    bool is_master_{false};

    // Client-side Wayland surface buffer
    std::shared_ptr<render::FrameBuffer> client_surface_;
    struct wl_display* wl_display_{nullptr};
};

} // namespace prism::sdk
