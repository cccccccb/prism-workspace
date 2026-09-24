#include "prism/sdk/application.hpp"
#include "prism/compiler/binary_loader.hpp"
#include "prism/scene/container_node.hpp"
#include "prism/scene/leaf_nodes.hpp"
#include "prism/render/canvas_renderer.hpp"
#include "prism/core/logging.hpp"
#include <wayland-client.h>
#include <cstdlib>
#include <thread>
#include <chrono>

namespace prism::sdk {

std::shared_ptr<Application> Application::Create(const AppConfig& config) {
    std::string ch_name = config.channel_name;
    if (ch_name.empty()) {
        const char* env_ch = std::getenv("PRISM_CHANNEL");
        if (env_ch) {
            ch_name = env_ch;
        } else {
            ch_name = "/" + config.app_id;
        }
    }

    auto channel = ipc::Channel::ConnectClient(ch_name);
    if (!channel) {
        PRISM_LOG_WARN("SDK", "[%s] Could not connect to IPC channel '%s'; running in standalone Wayland client mode",
                       config.app_id.c_str(), ch_name.c_str());
    } else {
        PRISM_LOG_INFO("SDK", "[%s] Connected to IPC channel: %s", config.app_id.c_str(), ch_name.c_str());
    }

    return std::shared_ptr<Application>(new Application(config, std::move(channel)));
}

std::shared_ptr<Application> Application::Connect(const std::string& channel_name) {
    AppConfig cfg{};
    cfg.app_id = "prism_app";
    cfg.channel_name = channel_name;
    return Create(cfg);
}

Application::Application(AppConfig config, std::shared_ptr<ipc::Channel> channel)
    : config_(std::move(config)), channel_(std::move(channel)) {
    InitializeWaylandClient();
    client_surface_ = std::make_shared<render::FrameBuffer>(config_.width, config_.height);
    LoadPackageDsl();
}

Application::~Application() {
    Exit(0);
    if (wl_display_) {
        wl_display_disconnect(wl_display_);
        wl_display_ = nullptr;
    }
}

void Application::InitializeWaylandClient() {
    std::string disp = config_.wayland_display;
    if (disp.empty()) {
        const char* env_disp = std::getenv("WAYLAND_DISPLAY");
        if (env_disp) disp = env_disp;
    }

    const char* target_socket = disp.empty() ? "wayland-prism-0" : disp.c_str();
    wl_display_ = wl_display_connect(target_socket);
    if (wl_display_) {
        PRISM_LOG_INFO("SDK-WAYLAND", "[%s] Connected to Wayland Compositor on socket '%s' (Client surface active)",
                       config_.app_id.c_str(), target_socket);
    } else {
        PRISM_LOG_INFO("SDK-WAYLAND", "[%s] Compositor socket '%s' not present; using shared client surface mode",
                       config_.app_id.c_str(), target_socket);
    }
}

void Application::IndexSlots(const std::shared_ptr<scene::SceneNode>& node) {
    if (!node) return;
    if (node->GetSlot() != 0) {
        slot_index_[node->GetSlot()] = node;
        PRISM_LOG_DEBUG("SDK-AST", "[%s] Indexed reactive slot 0x%08X on node '%s'",
                        config_.app_id.c_str(), node->GetSlot(), node->GetName().c_str());
    }
    if (auto container = std::dynamic_pointer_cast<scene::ContainerNode>(node)) {
        for (const auto& child : container->GetChildren()) {
            IndexSlots(child);
        }
    }
}

void Application::LoadPackageDsl() {
    if (config_.package_path.empty()) return;

    PRISM_LOG_INFO("SDK-DSL", "[%s] Client parsing DSL bundle from package: %s",
                   config_.app_id.c_str(), config_.package_path.c_str());

    preview_tree_ = compiler::BinarySceneLoader::LoadFromPackage(config_.package_path, "preview.prismb");
    if (!preview_tree_) {
        preview_tree_ = compiler::BinarySceneLoader::LoadFromFile(config_.package_path + ":preview.prismb");
    }

    master_tree_ = compiler::BinarySceneLoader::LoadFromPackage(config_.package_path, "master.prismb");
    if (!master_tree_) {
        master_tree_ = compiler::BinarySceneLoader::LoadFromFile(config_.package_path + ":master.prismb");
    }
    if (!master_tree_) {
        master_tree_ = compiler::BinarySceneLoader::LoadFromFile(config_.package_path);
    }

    if (master_tree_) {
        slot_index_.clear();
        IndexSlots(master_tree_);
        PRISM_LOG_INFO("SDK-DSL", "[%s] Client successfully parsed Master AST (%zu reactive slots indexed)",
                       config_.app_id.c_str(), slot_index_.size());
    }

    // Client-side initial render to surface
    RenderSurface();
}

void Application::RenderSurface() {
    if (!client_surface_) return;

    auto tree = is_master_ ? master_tree_ : (preview_tree_ ? preview_tree_ : master_tree_);
    if (!tree) return;

    client_surface_->Clear(0x00000000);
    render::CanvasRenderVisitor visitor(*client_surface_,
                                       core::Rect{0, 0, static_cast<float>(config_.width), static_cast<float>(config_.height)});
    tree->Accept(visitor);
}

void Application::On(const std::string& action, ActionHandler handler) {
    dispatcher_.Register(action, std::move(handler));
    PRISM_LOG_DEBUG("SDK", "Registered handler for action: %s", action.c_str());
}

void Application::SetState(std::string_view slot_name, std::string_view str) {
    uint32_t slot = core::HashSlot(slot_name);
    auto it = slot_index_.find(slot);
    if (it != slot_index_.end()) {
        if (auto text_node = std::dynamic_pointer_cast<scene::TextNode>(it->second)) {
            text_node->SetText(std::string(str));
        } else if (auto btn_node = std::dynamic_pointer_cast<scene::ButtonNode>(it->second)) {
            btn_node->SetLabel(std::string(str));
        } else if (auto input_node = std::dynamic_pointer_cast<scene::TextInputNode>(it->second)) {
            input_node->SetText(std::string(str));
        } else if (auto badge_node = std::dynamic_pointer_cast<scene::BadgeNode>(it->second)) {
            badge_node->SetText(std::string(str));
        }
    }

    if (channel_) {
        auto pkt = ipc::StateDiffPacket::MakeString(slot, str);
        channel_->PushStateDiff(pkt);
    }

    RenderSurface();
    PRISM_LOG_INFO("SDK", "[%s] SetState '%s' -> '%s' (slot: 0x%08X)",
                   config_.app_id.c_str(), std::string(slot_name).c_str(), std::string(str).c_str(), slot);
}

void Application::SetState(std::string_view slot_name, const char* str) {
    SetState(slot_name, std::string_view(str ? str : ""));
}

void Application::SetState(std::string_view slot_name, const std::string& str) {
    SetState(slot_name, std::string_view(str));
}

void Application::SetState(std::string_view slot_name, int64_t val) {
    uint32_t slot = core::HashSlot(slot_name);
    auto it = slot_index_.find(slot);
    if (it != slot_index_.end()) {
        if (auto slider = std::dynamic_pointer_cast<scene::SliderNode>(it->second)) {
            slider->SetValue(static_cast<double>(val));
        } else if (auto progress = std::dynamic_pointer_cast<scene::ProgressBarNode>(it->second)) {
            progress->SetProgress(static_cast<float>(val > 1 ? val / 100.0 : val));
        } else if (auto toggle = std::dynamic_pointer_cast<scene::ToggleNode>(it->second)) {
            toggle->SetState(val != 0);
        } else if (auto text = std::dynamic_pointer_cast<scene::TextNode>(it->second)) {
            text->SetText(std::to_string(val));
        } else if (auto badge = std::dynamic_pointer_cast<scene::BadgeNode>(it->second)) {
            badge->SetText(std::to_string(val));
        }
    }

    if (channel_) {
        auto pkt = ipc::StateDiffPacket::MakeInt(slot, val);
        channel_->PushStateDiff(pkt);
    }
    RenderSurface();
}

void Application::SetState(std::string_view slot_name, double val) {
    uint32_t slot = core::HashSlot(slot_name);
    auto it = slot_index_.find(slot);
    if (it != slot_index_.end()) {
        if (auto slider = std::dynamic_pointer_cast<scene::SliderNode>(it->second)) {
            slider->SetValue(val);
        } else if (auto progress = std::dynamic_pointer_cast<scene::ProgressBarNode>(it->second)) {
            progress->SetProgress(static_cast<float>(val > 1.0 ? val / 100.0 : val));
        } else if (auto toggle = std::dynamic_pointer_cast<scene::ToggleNode>(it->second)) {
            toggle->SetState(val > 0.5);
        } else if (auto text = std::dynamic_pointer_cast<scene::TextNode>(it->second)) {
            text->SetText(std::to_string(val));
        }
    }

    if (channel_) {
        auto pkt = ipc::StateDiffPacket::MakeFloat(slot, val);
        channel_->PushStateDiff(pkt);
    }
    RenderSurface();
}

void Application::SetState(std::string_view slot_name, bool val) {
    uint32_t slot = core::HashSlot(slot_name);
    auto it = slot_index_.find(slot);
    if (it != slot_index_.end()) {
        if (auto toggle = std::dynamic_pointer_cast<scene::ToggleNode>(it->second)) {
            toggle->SetState(val);
        } else if (auto text = std::dynamic_pointer_cast<scene::TextNode>(it->second)) {
            text->SetText(val ? "true" : "false");
        }
    }

    if (channel_) {
        auto pkt = ipc::StateDiffPacket::MakeBool(slot, val);
        channel_->PushStateDiff(pkt);
    }
    RenderSurface();
}

bool Application::HotReload(const std::string& package_path) {
    if (!package_path.empty()) {
        config_.package_path = package_path;
    }
    if (config_.package_path.empty()) return false;

    PRISM_LOG_INFO("SDK-DSL", "[%s] Hot-reloading DSL bundle from: %s",
                   config_.app_id.c_str(), config_.package_path.c_str());

    // Preserve runtime state across hot-reload
    std::unordered_map<uint32_t, std::string> saved_text;
    std::unordered_map<uint32_t, double> saved_double;
    std::unordered_map<uint32_t, bool> saved_bool;

    for (const auto& [slot, node] : slot_index_) {
        if (auto text = std::dynamic_pointer_cast<scene::TextNode>(node)) {
            saved_text[slot] = text->GetText();
        } else if (auto input = std::dynamic_pointer_cast<scene::TextInputNode>(node)) {
            saved_text[slot] = input->GetText();
        } else if (auto badge = std::dynamic_pointer_cast<scene::BadgeNode>(node)) {
            saved_text[slot] = badge->GetText();
        } else if (auto slider = std::dynamic_pointer_cast<scene::SliderNode>(node)) {
            saved_double[slot] = slider->GetValue();
        } else if (auto progress = std::dynamic_pointer_cast<scene::ProgressBarNode>(node)) {
            saved_double[slot] = progress->GetProgress();
        } else if (auto toggle = std::dynamic_pointer_cast<scene::ToggleNode>(node)) {
            saved_bool[slot] = toggle->IsOn();
        }
    }

    // Reload AST from updated bundle
    LoadPackageDsl();

    // Reapply runtime states to the fresh AST
    for (const auto& [slot, str] : saved_text) {
        auto it = slot_index_.find(slot);
        if (it != slot_index_.end()) {
            if (auto t = std::dynamic_pointer_cast<scene::TextNode>(it->second)) t->SetText(str);
            else if (auto inp = std::dynamic_pointer_cast<scene::TextInputNode>(it->second)) inp->SetText(str);
            else if (auto b = std::dynamic_pointer_cast<scene::BadgeNode>(it->second)) b->SetText(str);
        }
    }
    for (const auto& [slot, val] : saved_double) {
        auto it = slot_index_.find(slot);
        if (it != slot_index_.end()) {
            if (auto s = std::dynamic_pointer_cast<scene::SliderNode>(it->second)) s->SetValue(val);
            else if (auto p = std::dynamic_pointer_cast<scene::ProgressBarNode>(it->second)) p->SetProgress(static_cast<float>(val));
        }
    }
    for (const auto& [slot, val] : saved_bool) {
        auto it = slot_index_.find(slot);
        if (it != slot_index_.end()) {
            if (auto tog = std::dynamic_pointer_cast<scene::ToggleNode>(it->second)) tog->SetState(val);
        }
    }

    RenderSurface();
    PRISM_LOG_INFO("SDK-DSL", "[%s] Hot-reload complete with runtime state preserved (%zu slots)",
                   config_.app_id.c_str(), slot_index_.size());
    return true;
}

void Application::Ready() {
    is_master_ = true;
    RenderSurface();

    if (channel_) {
        auto pkt = ipc::StateDiffPacket::MakeReady();
        channel_->PushStateDiff(pkt);
        auto* layout = channel_->GetLayout();
        if (layout) {
            layout->header.is_master_ready.store(true);
        }
    }
    PRISM_LOG_INFO("SDK", "[%s] Client surface rendered Master UI and signaled READY! (0ms -> Master complete)",
                   config_.app_id.c_str());
}

void Application::PollEvents() {
    if (!channel_) return;
    ipc::EventPacket event;
    while (channel_->PopEvent(event)) {
        if (!dispatcher_.Dispatch(event)) {
            PRISM_LOG_WARN("SDK", "[%s] Unhandled event action: %s", config_.app_id.c_str(), event.action);
        }
    }
}

int Application::Exec() {
    running_ = true;
    PRISM_LOG_INFO("SDK", "[%s] Entering Wayland client application event loop...", config_.app_id.c_str());

    while (running_) {
        PollEvents();
        if (wl_display_) {
            wl_display_dispatch_pending(wl_display_);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return exit_code_;
}

void Application::Exit(int code) {
    if (running_) {
        running_ = false;
        exit_code_ = code;
        if (channel_) {
            auto pkt = ipc::StateDiffPacket::MakeExit();
            channel_->PushStateDiff(pkt);
        }
        PRISM_LOG_INFO("SDK", "[%s] Application exited with code %d", config_.app_id.c_str(), code);
    }
}

} // namespace prism::sdk
