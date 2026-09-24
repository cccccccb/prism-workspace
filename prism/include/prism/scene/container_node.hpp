#pragma once

#include "prism/scene/node.hpp"
#include <vector>

namespace prism::scene {

class ContainerNode : public SceneNode {
public:
    explicit ContainerNode(std::string name = "Container")
        : SceneNode(std::move(name)) {}

    bool IsContainer() const override { return true; }

    void AddChild(std::shared_ptr<SceneNode> child) {
        if (child) {
            child->SetParent(shared_from_this());
            children_.push_back(std::move(child));
        }
    }

    const std::vector<std::shared_ptr<SceneNode>>& GetChildren() const {
        return children_;
    }

    void ClearChildren() {
        children_.clear();
    }

protected:
    std::vector<std::shared_ptr<SceneNode>> children_;
};

class VStackNode : public ContainerNode {
public:
    explicit VStackNode(float spacing = 8.0f, std::string name = "VStack")
        : ContainerNode(std::move(name)), spacing_(spacing) {}

    void Accept(SceneVisitor& visitor) override {
        visitor.Visit(*this);
    }

    float GetSpacing() const { return spacing_; }

private:
    float spacing_{8.0f};
};

class HStackNode : public ContainerNode {
public:
    explicit HStackNode(float spacing = 8.0f, std::string name = "HStack")
        : ContainerNode(std::move(name)), spacing_(spacing) {}

    void Accept(SceneVisitor& visitor) override {
        visitor.Visit(*this);
    }

    float GetSpacing() const { return spacing_; }

private:
    float spacing_{8.0f};
};

class CardNode : public ContainerNode {
public:
    explicit CardNode(float spacing = 12.0f, std::string name = "Card")
        : ContainerNode(std::move(name)), spacing_(spacing) {}

    void Accept(SceneVisitor& visitor) override {
        visitor.Visit(*this);
    }

    float GetSpacing() const { return spacing_; }

private:
    float spacing_{12.0f};
};

class ZStackNode : public ContainerNode {
public:
    explicit ZStackNode(std::string name = "ZStack")
        : ContainerNode(std::move(name)) {}

    void Accept(SceneVisitor& visitor) override {
        visitor.Visit(*this);
    }
};

} // namespace prism::scene
