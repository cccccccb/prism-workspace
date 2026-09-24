#pragma once

#include "prism/core/types.hpp"
#include "prism/modifiers/modifier_chain.hpp"
#include "prism/scene/scene_visitor.hpp"
#include <string>
#include <memory>
#include <vector>

namespace prism::scene {

/**
 * @brief Composite Pattern: Base component in the hierarchical scene graph
 */
class SceneNode : public std::enable_shared_from_this<SceneNode> {
public:
    explicit SceneNode(std::string name = "")
        : name_(std::move(name)) {}

    virtual ~SceneNode() = default;

    virtual void Accept(SceneVisitor& visitor) = 0;
    virtual bool IsContainer() const { return false; }

    const std::string& GetName() const { return name_; }
    void SetName(std::string name) { name_ = std::move(name); }

    core::Rect& Bounds() { return bounds_; }
    const core::Rect& Bounds() const { return bounds_; }

    modifiers::ModifierChain& Modifiers() { return modifiers_; }
    const modifiers::ModifierChain& Modifiers() const { return modifiers_; }

    void SetSlot(uint32_t slot_id) { bound_slot_ = slot_id; }
    uint32_t GetSlot() const { return bound_slot_; }

    // Parent reference
    void SetParent(std::weak_ptr<SceneNode> parent) { parent_ = parent; }
    std::shared_ptr<SceneNode> GetParent() const { return parent_.lock(); }

private:
    std::string name_;
    core::Rect bounds_;
    modifiers::ModifierChain modifiers_;
    uint32_t bound_slot_{0};
    std::weak_ptr<SceneNode> parent_;
};

} // namespace prism::scene
