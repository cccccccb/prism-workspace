#pragma once

namespace prism::scene {

class SceneNode;
class ContainerNode;
class VStackNode;
class HStackNode;
class ZStackNode;
class CardNode;
class TextNode;
class ButtonNode;
class SliderNode;
class SkeletonNode;
class IconNode;
class ToggleNode;
class TextInputNode;
class ProgressBarNode;
class BadgeNode;
class SpacerNode;

/**
 * @brief Visitor Pattern: Decouples rendering and tree inspection from the scene graph node classes
 */
class SceneVisitor {
public:
    virtual ~SceneVisitor() = default;

    virtual void Visit(VStackNode& node) = 0;
    virtual void Visit(HStackNode& node) = 0;
    virtual void Visit(ZStackNode& node) = 0;
    virtual void Visit(CardNode& node) = 0;
    virtual void Visit(TextNode& node) = 0;
    virtual void Visit(ButtonNode& node) = 0;
    virtual void Visit(SliderNode& node) = 0;
    virtual void Visit(SkeletonNode& node) = 0;
    virtual void Visit(IconNode& node) = 0;
    virtual void Visit(ToggleNode& node) = 0;
    virtual void Visit(TextInputNode& node) = 0;
    virtual void Visit(ProgressBarNode& node) = 0;
    virtual void Visit(BadgeNode& node) = 0;
    virtual void Visit(SpacerNode& node) = 0;
};

} // namespace prism::scene
