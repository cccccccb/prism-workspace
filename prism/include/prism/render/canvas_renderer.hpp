#pragma once

#include "prism/scene/scene_visitor.hpp"
#include "prism/render/framebuffer.hpp"
#include "prism/core/types.hpp"
#include <memory>

namespace prism::render {

/**
 * @brief Visitor Pattern: Rasterizes the composite scene graph directly onto the FrameBuffer
 */
class CanvasRenderVisitor : public scene::SceneVisitor {
public:
    CanvasRenderVisitor(FrameBuffer& fb, core::Rect viewport);

    void Visit(scene::VStackNode& node) override;
    void Visit(scene::HStackNode& node) override;
    void Visit(scene::ZStackNode& node) override;
    void Visit(scene::CardNode& node) override;
    void Visit(scene::DesktopNode& node) override;
    void Visit(scene::TopBarNode& node) override;
    void Visit(scene::DockNode& node) override;
    void Visit(scene::AppGroupNode& node) override;
    void Visit(scene::TextNode& node) override;
    void Visit(scene::ButtonNode& node) override;
    void Visit(scene::SliderNode& node) override;
    void Visit(scene::SkeletonNode& node) override;
    void Visit(scene::IconNode& node) override;
    void Visit(scene::ToggleNode& node) override;
    void Visit(scene::TextInputNode& node) override;
    void Visit(scene::ProgressBarNode& node) override;
    void Visit(scene::BadgeNode& node) override;
    void Visit(scene::SpacerNode& node) override;

    void SetViewport(core::Rect vp) { viewport_ = vp; }

private:
    void RenderModifiers(scene::SceneNode& node, const core::Rect& rect);

    FrameBuffer& fb_;
    core::Rect viewport_;
    float cursor_x_{0.0f};
    float cursor_y_{0.0f};
};

} // namespace prism::render
