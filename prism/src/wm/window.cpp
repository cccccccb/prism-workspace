#include "prism/wm/window.hpp"
#include "prism/lifecycle/preview_state.hpp"
#include "prism/scene/container_node.hpp"
#include "prism/scene/leaf_nodes.hpp"
#include "prism/core/logging.hpp"
#include "prism/render/framebuffer.hpp"
#include <iostream>

namespace prism::wm {

// Visitor Pattern implementation for visual tree inspection and rendering
class ConsoleRenderVisitor : public scene::SceneVisitor {
public:
    explicit ConsoleRenderVisitor(int indent = 1) : indent_(indent) {}

    void Visit(scene::VStackNode& node) override {
        PrintPrefix(node);
        std::cout << "[VStack spacing=" << node.GetSpacing() << "]\n";
        Recurse(node);
    }

    void Visit(scene::HStackNode& node) override {
        PrintPrefix(node);
        std::cout << "[HStack spacing=" << node.GetSpacing() << "]\n";
        Recurse(node);
    }

    void Visit(scene::TextNode& node) override {
        PrintPrefix(node);
        std::cout << "[Text: \"" << node.GetText() << "\" font=" << node.GetFontSize() << "]\n";
    }

    void Visit(scene::ButtonNode& node) override {
        PrintPrefix(node);
        std::cout << "[Button: \"" << node.GetLabel() << "\" action=" << node.GetAction() << "]\n";
    }

    void Visit(scene::SliderNode& node) override {
        PrintPrefix(node);
        std::cout << "[Slider: value=" << node.GetValue() << "]\n";
    }

    void Visit(scene::SkeletonNode& node) override {
        PrintPrefix(node);
        std::cout << "[Skeleton: shimmer=" << node.GetShimmerSpeed() << "s text=\"" << node.GetLabel() << "\"]\n";
    }

    void Visit(scene::IconNode& node) override {
        PrintPrefix(node);
        std::cout << "[Icon: " << node.GetIconName() << " scale=" << node.GetScale() << "]\n";
    }

private:
    void PrintPrefix(scene::SceneNode& node) {
        std::string pad(indent_ * 2, ' ');
        std::cout << pad << "• (" << node.GetName() << ") ";
        for (const auto& mod : node.Modifiers().GetAll()) {
            std::cout << "{" << mod->Describe() << "} ";
        }
    }

    void Recurse(scene::ContainerNode& container) {
        indent_++;
        for (const auto& child : container.GetChildren()) {
            if (child) child->Accept(*this);
        }
        indent_--;
    }

    int indent_{1};
};

Window::Window(std::string app_id, std::string title, core::Rect bounds, std::shared_ptr<ipc::Channel> channel)
    : app_id_(std::move(app_id)), title_(std::move(title)), bounds_(bounds), channel_(std::move(channel)) {
    current_state_ = std::make_unique<lifecycle::PreviewState>();
    current_state_->OnEnter(*this);
}

Window::~Window() = default;

void Window::SetPreviewTree(std::shared_ptr<scene::SceneNode> preview) {
    preview_tree_ = std::move(preview);
    PRISM_LOG_INFO("WM-WIN", "[%s] Preview scene tree mounted", app_id_.c_str());
}

void Window::AttachSurface(std::shared_ptr<render::FrameBuffer> surface) {
    surface_buffer_ = std::move(surface);
    PRISM_LOG_INFO("WM-WIN", "[%s] Attached Wayland client surface buffer (%dx%d)",
                   app_id_.c_str(),
                   surface_buffer_ ? surface_buffer_->GetWidth() : 0,
                   surface_buffer_ ? surface_buffer_->GetHeight() : 0);
}

void Window::IndexSlots(const std::shared_ptr<scene::SceneNode>& node) {
    if (!node) return;
    if (node->GetSlot() != 0) {
        slot_index_[node->GetSlot()] = node;
        PRISM_LOG_INFO("WM-WIN", "Indexed reactive slot 0x%08X on node '%s'", node->GetSlot(), node->GetName().c_str());
    }
    if (auto container = std::dynamic_pointer_cast<scene::ContainerNode>(node)) {
        for (const auto& child : container->GetChildren()) {
            IndexSlots(child);
        }
    }
}

void Window::SetMasterTree(std::shared_ptr<scene::SceneNode> master) {
    master_tree_ = std::move(master);
    slot_index_.clear();
    IndexSlots(master_tree_);
    PRISM_LOG_INFO("WM-WIN", "[%s] Master scene tree mounted (Indexed %zu reactive slots)", app_id_.c_str(), slot_index_.size());
}

void Window::TransitionTo(std::unique_ptr<lifecycle::WindowState> new_state) {
    current_state_ = std::move(new_state);
    if (current_state_) {
        current_state_->OnEnter(*this);
    }
}

void Window::Tick(float dt) {
    if (current_state_) {
        current_state_->Tick(*this, dt);
    }
}

void Window::OnMasterReady() {
    if (current_state_) {
        current_state_->OnMasterReady(*this);
    }
}

void Window::UpdateSlot(uint32_t slot_id, const std::string& val) {
    auto it = slot_index_.find(slot_id);
    if (it != slot_index_.end()) {
        if (auto text_node = std::dynamic_pointer_cast<scene::TextNode>(it->second)) {
            text_node->SetText(val);
        } else if (auto btn_node = std::dynamic_pointer_cast<scene::ButtonNode>(it->second)) {
            btn_node->SetLabel(val);
        }
        PRISM_LOG_INFO("WM-WIN", "[%s] Slot 0x%08X updated to string '%s'", app_id_.c_str(), slot_id, val.c_str());
    } else {
        PRISM_LOG_WARN("WM-WIN", "[%s] String slot 0x%08X not found in index!", app_id_.c_str(), slot_id);
    }
}

void Window::UpdateSlot(uint32_t slot_id, double val) {
    auto it = slot_index_.find(slot_id);
    if (it != slot_index_.end()) {
        if (auto slider = std::dynamic_pointer_cast<scene::SliderNode>(it->second)) {
            slider->SetValue(val);
        } else if (auto text_node = std::dynamic_pointer_cast<scene::TextNode>(it->second)) {
            text_node->SetText(std::to_string(val));
        }
        PRISM_LOG_DEBUG("WM-WIN", "[%s] Slot 0x%08X updated to %.2f", app_id_.c_str(), slot_id, val);
    } else {
        PRISM_LOG_WARN("WM-WIN", "[%s] Float slot 0x%08X not found in index!", app_id_.c_str(), slot_id);
    }
}

void Window::Render() {
    ConsoleRenderVisitor visitor(1);

    std::cout << "┌── [Prism Window: " << title_ << " (" << bounds_.width << "x" << bounds_.height << ")] ";
    std::cout << "<State: " << (current_state_ ? current_state_->GetStateName() : "None") << "> ──┐\n";

    if (preview_tree_) {
        preview_tree_->Accept(visitor);
    } else if (master_tree_) {
        master_tree_->Accept(visitor);
    }

    std::cout << "└──" << std::string(50, '-') << "──┘\n";
}

} // namespace prism::wm
