#pragma once

#include "prism/scene/node.hpp"

namespace prism::scene {

class TextNode : public SceneNode {
public:
    explicit TextNode(std::string text = "", std::string name = "Text")
        : SceneNode(std::move(name)), text_(std::move(text)) {}

    void Accept(SceneVisitor& visitor) override {
        visitor.Visit(*this);
    }

    const std::string& GetText() const { return text_; }
    void SetText(std::string text) { text_ = std::move(text); }

    float GetFontSize() const { return font_size_; }
    void SetFontSize(float size) { font_size_ = size; }

    core::Color GetColor() const { return color_; }
    void SetColor(core::Color c) { color_ = c; }

private:
    std::string text_;
    float font_size_{14.0f};
    core::Color color_{255, 255, 255, 255};
};

class ButtonNode : public SceneNode {
public:
    ButtonNode(std::string label, std::string action, std::string name = "Button")
        : SceneNode(std::move(name)), label_(std::move(label)), action_(std::move(action)) {}

    void Accept(SceneVisitor& visitor) override {
        visitor.Visit(*this);
    }

    const std::string& GetLabel() const { return label_; }
    void SetLabel(std::string label) { label_ = std::move(label); }

    const std::string& GetAction() const { return action_; }
    void SetAction(std::string action) { action_ = std::move(action); }

private:
    std::string label_;
    std::string action_;
};

class SliderNode : public SceneNode {
public:
    explicit SliderNode(double value = 0.0, std::string name = "Slider")
        : SceneNode(std::move(name)), value_(value) {}

    void Accept(SceneVisitor& visitor) override {
        visitor.Visit(*this);
    }

    double GetValue() const { return value_; }
    void SetValue(double val) { value_ = val; }

private:
    double value_{0.0};
};

class SkeletonNode : public SceneNode {
public:
    explicit SkeletonNode(std::string label = "Loading...", float shimmer_speed = 1.0f, std::string name = "Skeleton")
        : SceneNode(std::move(name)), label_(std::move(label)), shimmer_speed_(shimmer_speed) {}

    void Accept(SceneVisitor& visitor) override {
        visitor.Visit(*this);
    }

    const std::string& GetLabel() const { return label_; }
    float GetShimmerSpeed() const { return shimmer_speed_; }

private:
    std::string label_;
    float shimmer_speed_{1.0f};
};

class IconNode : public SceneNode {
public:
    explicit IconNode(std::string icon_name, float scale = 1.0f, std::string name = "Icon")
        : SceneNode(std::move(name)), icon_name_(std::move(icon_name)), scale_(scale) {}

    void Accept(SceneVisitor& visitor) override {
        visitor.Visit(*this);
    }

    const std::string& GetIconName() const { return icon_name_; }
    float GetScale() const { return scale_; }
    void SetScale(float s) { scale_ = s; }

private:
    std::string icon_name_;
    float scale_{1.0f};
};

class ToggleNode : public SceneNode {
public:
    ToggleNode(bool state = false, std::string label = "", std::string action = "", std::string name = "Toggle")
        : SceneNode(std::move(name)), state_(state), label_(std::move(label)), action_(std::move(action)) {}

    void Accept(SceneVisitor& visitor) override {
        visitor.Visit(*this);
    }

    bool GetState() const { return state_; }
    bool IsOn() const { return state_; }
    void SetState(bool s) { state_ = s; }

    const std::string& GetLabel() const { return label_; }
    void SetLabel(std::string l) { label_ = std::move(l); }

    const std::string& GetAction() const { return action_; }
    void SetAction(std::string a) { action_ = std::move(a); }

private:
    bool state_{false};
    std::string label_;
    std::string action_;
};

class TextInputNode : public SceneNode {
public:
    TextInputNode(std::string text = "", std::string placeholder = "Enter text...", std::string action = "", std::string name = "TextInput")
        : SceneNode(std::move(name)), text_(std::move(text)), placeholder_(std::move(placeholder)), action_(std::move(action)) {}

    void Accept(SceneVisitor& visitor) override {
        visitor.Visit(*this);
    }

    const std::string& GetText() const { return text_; }
    void SetText(std::string t) { text_ = std::move(t); }

    const std::string& GetPlaceholder() const { return placeholder_; }
    void SetPlaceholder(std::string p) { placeholder_ = std::move(p); }

    const std::string& GetAction() const { return action_; }
    void SetAction(std::string a) { action_ = std::move(a); }

    bool IsFocused() const { return focused_; }
    void SetFocused(bool f) { focused_ = f; }

private:
    std::string text_;
    std::string placeholder_;
    std::string action_;
    bool focused_{false};
};

class ProgressBarNode : public SceneNode {
public:
    explicit ProgressBarNode(double progress = 0.0, std::string name = "ProgressBar")
        : SceneNode(std::move(name)), progress_(progress) {}

    void Accept(SceneVisitor& visitor) override {
        visitor.Visit(*this);
    }

    double GetProgress() const { return progress_; }
    void SetProgress(double p) { progress_ = std::max(0.0, std::min(1.0, p)); }

    core::Color GetTint() const { return tint_; }
    void SetTint(core::Color c) { tint_ = c; }

private:
    double progress_{0.0};
    core::Color tint_{0, 122, 255, 255}; // Default blue
};

class BadgeNode : public SceneNode {
public:
    BadgeNode(std::string text = "", core::Color color = {0, 122, 255, 255}, std::string name = "Badge")
        : SceneNode(std::move(name)), text_(std::move(text)), color_(color) {}
    BadgeNode(std::string text, std::string name)
        : SceneNode(std::move(name)), text_(std::move(text)), color_{0, 122, 255, 255} {}

    void Accept(SceneVisitor& visitor) override {
        visitor.Visit(*this);
    }

    const std::string& GetText() const { return text_; }
    void SetText(std::string t) { text_ = std::move(t); }

    core::Color GetColor() const { return color_; }
    void SetColor(core::Color c) { color_ = c; }

private:
    std::string text_;
    core::Color color_{0, 122, 255, 255};
};

class SpacerNode : public SceneNode {
public:
    explicit SpacerNode(float min_length = 0.0f, std::string name = "Spacer")
        : SceneNode(std::move(name)), min_length_(min_length) {}

    void Accept(SceneVisitor& visitor) override {
        visitor.Visit(*this);
    }

    float GetMinLength() const { return min_length_; }
    void SetMinLength(float len) { min_length_ = len; }

private:
    float min_length_{0.0f};
};

} // namespace prism::scene
