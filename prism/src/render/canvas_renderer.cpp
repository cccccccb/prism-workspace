#include "prism/render/canvas_renderer.hpp"
#include "prism/scene/container_node.hpp"
#include "prism/scene/leaf_nodes.hpp"
#include "prism/modifiers/blur_modifier.hpp"
#include "prism/modifiers/geometry_modifier.hpp"

namespace prism::render {

CanvasRenderVisitor::CanvasRenderVisitor(FrameBuffer& fb, core::Rect viewport)
    : fb_(fb), viewport_(viewport), cursor_x_(viewport.x), cursor_y_(viewport.y) {}

void CanvasRenderVisitor::RenderModifiers(scene::SceneNode& node, const core::Rect& rect) {
    auto& mods = node.Modifiers();

    int rx = static_cast<int>(rect.x);
    int ry = static_cast<int>(rect.y);
    int rw = static_cast<int>(rect.width);
    int rh = static_cast<int>(rect.height);

    // 1. Shadow modifier
    if (auto shadow = mods.Find<modifiers::ShadowModifier>()) {
        fb_.DrawShadow(rx, ry + static_cast<int>(shadow->GetYOffset()), rw, rh, 18.0f, shadow->GetRadius(), 0x77000000);
    }

    // 2. Dual Kawase Blur modifier (Mac Glass background)
    if (auto blur = mods.Find<modifiers::KawaseBlurModifier>()) {
        fb_.ApplyKawaseBlur(rx, ry, rw, rh, blur->GetRadius(), blur->GetPasses());
    }

    // 3. Rounded Rect Glass Tint (Only for styled surfaces)
    if (mods.Find<modifiers::CornerRadiusModifier>() || mods.Find<modifiers::KawaseBlurModifier>()) {
        float cr = 16.0f;
        if (auto corner = mods.Find<modifiers::CornerRadiusModifier>()) {
            cr = corner->GetRadius();
        }
        fb_.DrawRoundedRect(rx, ry, rw, rh, cr, 0x44282a36, 0.85f); // Subtle dark glass tint
        fb_.DrawRoundedRect(rx, ry, rw, rh, cr, 0x22ffffff, 0.15f); // Top rim specular highlight
    }
}

void CanvasRenderVisitor::Visit(scene::VStackNode& node) {
    RenderModifiers(node, viewport_);

    float pad = 16.0f;
    if (auto p = node.Modifiers().Find<modifiers::PaddingModifier>()) {
        pad = p->GetTop();
    }

    float cur_y = viewport_.y + pad;
    float content_w = viewport_.width - pad * 2;

    for (const auto& child : node.GetChildren()) {
        if (!child) continue;
        core::Rect child_vp{viewport_.x + pad, cur_y, content_w, 48.0f};
        CanvasRenderVisitor child_visitor(fb_, child_vp);
        child->Accept(child_visitor);
        cur_y += 48.0f + node.GetSpacing();
    }
}

void CanvasRenderVisitor::Visit(scene::HStackNode& node) {
    RenderModifiers(node, viewport_);

    float pad = 16.0f;
    if (auto p = node.Modifiers().Find<modifiers::PaddingModifier>()) {
        pad = p->GetLeft();
    }

    float cur_x = viewport_.x + pad;
    size_t child_count = node.GetChildren().size();
    float child_w = (child_count > 0) ? (viewport_.width - pad * 2 - (child_count - 1) * node.GetSpacing()) / child_count : 100.0f;

    for (const auto& child : node.GetChildren()) {
        if (!child) continue;
        core::Rect child_vp{cur_x, viewport_.y + pad, child_w, viewport_.height - pad * 2};
        CanvasRenderVisitor child_visitor(fb_, child_vp);
        child->Accept(child_visitor);
        cur_x += child_w + node.GetSpacing();
    }
}

void CanvasRenderVisitor::Visit(scene::TextNode& node) {
    fb_.DrawTextSimple(static_cast<int>(viewport_.x), static_cast<int>(viewport_.y + 12), node.GetText(), 0xFFE0E0E0);
}

void CanvasRenderVisitor::Visit(scene::ButtonNode& node) {
    int bx = static_cast<int>(viewport_.x);
    int by = static_cast<int>(viewport_.y);
    int bw = static_cast<int>(viewport_.width);
    int bh = 36;

    // Mac Pill-shaped button
    fb_.DrawRoundedRect(bx, by, bw, bh, 8.0f, 0x663d4452);
    fb_.DrawTextSimple(bx + 12, by + 12, node.GetLabel(), 0xFFFFFFFF);
}

void CanvasRenderVisitor::Visit(scene::SliderNode& node) {
    int sx = static_cast<int>(viewport_.x);
    int sy = static_cast<int>(viewport_.y + 14);
    int sw = static_cast<int>(viewport_.width);

    // Track
    fb_.DrawRoundedRect(sx, sy, sw, 8, 4.0f, 0x55444455);
    // Filled progress
    int fw = static_cast<int>(sw * std::max(0.0, std::min(1.0, node.GetValue())));
    if (fw > 0) {
        fb_.DrawRoundedRect(sx, sy, fw, 8, 4.0f, 0xFF007AFF); // Apple Blue
    }
}

void CanvasRenderVisitor::Visit(scene::SkeletonNode& node) {
    int sx = static_cast<int>(viewport_.x);
    int sy = static_cast<int>(viewport_.y);
    int sw = static_cast<int>(viewport_.width);

    fb_.DrawRoundedRect(sx, sy, sw, 40, 10.0f, 0x33667788);
    fb_.DrawTextSimple(sx + 14, sy + 14, node.GetLabel(), 0xFFAAAAAA);
}

void CanvasRenderVisitor::Visit(scene::IconNode& node) {
    int ix = static_cast<int>(viewport_.x + viewport_.width * 0.5f - 24);
    int iy = static_cast<int>(viewport_.y + 10);
    fb_.DrawRoundedRect(ix, iy, 48, 48, 12.0f, 0xFF0A84FF); // App Icon squircle
}

} // namespace prism::render
