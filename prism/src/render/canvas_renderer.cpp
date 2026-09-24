#include "prism/render/canvas_renderer.hpp"
#include "prism/scene/container_node.hpp"
#include "prism/scene/leaf_nodes.hpp"
#include "prism/modifiers/blur_modifier.hpp"
#include "prism/modifiers/geometry_modifier.hpp"
#include "prism/modifiers/acrylic_modifier.hpp"
#include "prism/modifiers/glow_modifier.hpp"
#include "prism/modifiers/spring_hover_modifier.hpp"

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

    // 2. Glow modifier (Neon / Ambient atmosphere)
    if (auto glow = mods.Find<modifiers::GlowModifier>()) {
        auto c = glow->GetColor();
        uint32_t col = (static_cast<uint32_t>(c.a) << 24) | (static_cast<uint32_t>(c.b) << 16) | (static_cast<uint32_t>(c.g) << 8) | c.r;
        fb_.DrawGlow(rx, ry, rw, rh, glow->GetRadius(), col);
    }

    // 3. Dual Kawase Blur & Acrylic Modifier
    if (auto acrylic = mods.Find<modifiers::AcrylicModifier>()) {
        fb_.ApplyKawaseBlur(rx, ry, rw, rh, acrylic->GetBlurRadius(), acrylic->GetPasses());
        auto c = acrylic->GetTint();
        uint32_t col = (static_cast<uint32_t>(c.a) << 24) | (static_cast<uint32_t>(c.b) << 16) | (static_cast<uint32_t>(c.g) << 8) | c.r;
        float cr = 14.0f;
        if (auto corner = mods.Find<modifiers::CornerRadiusModifier>()) cr = corner->GetRadius();
        fb_.DrawRoundedRect(rx, ry, rw, rh, cr, col);
        fb_.DrawBorder(rx, ry, rw, rh, cr, 1.0f, 0x33FFFFFF); // Top rim subtle specular
    } else if (auto blur = mods.Find<modifiers::KawaseBlurModifier>()) {
        fb_.ApplyKawaseBlur(rx, ry, rw, rh, blur->GetRadius(), blur->GetPasses());
    }

    // 4. Rounded Rect Glass Tint (Only for styled surfaces)
    if (!mods.Find<modifiers::AcrylicModifier>() && (mods.Find<modifiers::CornerRadiusModifier>() || mods.Find<modifiers::KawaseBlurModifier>())) {
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

void CanvasRenderVisitor::Visit(scene::ZStackNode& node) {
    RenderModifiers(node, viewport_);
    for (const auto& child : node.GetChildren()) {
        if (!child) continue;
        CanvasRenderVisitor child_visitor(fb_, viewport_);
        child->Accept(child_visitor);
    }
}

void CanvasRenderVisitor::Visit(scene::CardNode& node) {
    int cx = static_cast<int>(viewport_.x);
    int cy = static_cast<int>(viewport_.y);
    int cw = static_cast<int>(viewport_.width);
    int ch = static_cast<int>(viewport_.height);

    // Default card glass styling if not overridden by modifiers
    if (node.Modifiers().GetAll().empty()) {
        fb_.DrawShadow(cx, cy + 4, cw, ch, 14.0f, 16.0f, 0x66000000);
        fb_.DrawRoundedRect(cx, cy, cw, ch, 14.0f, 0x551E2028); // Subtle dark acrylic
        fb_.DrawBorder(cx, cy, cw, ch, 14.0f, 1.0f, 0x22FFFFFF); // Specular rim
    } else {
        RenderModifiers(node, viewport_);
    }

    float pad = 14.0f;
    float cur_y = viewport_.y + pad;
    float content_w = viewport_.width - pad * 2.0f;

    for (const auto& child : node.GetChildren()) {
        if (!child) continue;
        core::Rect child_vp{viewport_.x + pad, cur_y, content_w, 42.0f};
        CanvasRenderVisitor child_visitor(fb_, child_vp);
        child->Accept(child_visitor);
        cur_y += 42.0f + node.GetSpacing();
    }
}

void CanvasRenderVisitor::Visit(scene::DesktopNode& node) {
    RenderModifiers(node, viewport_);
    for (const auto& child : node.GetChildren()) {
        if (!child) continue;
        CanvasRenderVisitor child_visitor(fb_, viewport_);
        child->Accept(child_visitor);
    }
}

void CanvasRenderVisitor::Visit(scene::TopBarNode& node) {
    int bx = static_cast<int>(viewport_.x);
    int by = static_cast<int>(viewport_.y);
    int bw = static_cast<int>(viewport_.width);
    int bh = static_cast<int>(viewport_.height);

    if (node.Modifiers().GetAll().empty()) {
        // macOS Top Menu Bar Glass Default
        fb_.DrawRoundedRect(bx, by, bw, bh, 0.0f, 0xD4161922);
        fb_.DrawRect(bx, by + bh - 1, bw, 1, 0x22FFFFFF); // Subtly lit bottom separator
    } else {
        RenderModifiers(node, viewport_);
    }

    float pad = 12.0f;
    float cur_x = viewport_.x + pad;
    size_t count = node.GetChildren().size();
    float item_w = (count > 0) ? (viewport_.width - pad * 2.0f - (count - 1) * node.GetSpacing()) / count : 120.0f;

    for (const auto& child : node.GetChildren()) {
        if (!child) continue;
        core::Rect child_vp{cur_x, viewport_.y, item_w, viewport_.height};
        CanvasRenderVisitor child_visitor(fb_, child_vp);
        child->Accept(child_visitor);
        cur_x += item_w + node.GetSpacing();
    }
}

void CanvasRenderVisitor::Visit(scene::DockNode& node) {
    int dx = static_cast<int>(viewport_.x);
    int dy = static_cast<int>(viewport_.y);
    int dw = static_cast<int>(viewport_.width);
    int dh = static_cast<int>(viewport_.height);

    if (node.Modifiers().GetAll().empty()) {
        // macOS Floating Pill Dock Default
        fb_.DrawShadow(dx, dy + 2, dw, dh, 18.0f, 20.0f, 0x88000000);
        fb_.DrawRoundedRect(dx, dy, dw, dh, 18.0f, 0xEE12151E);
        fb_.DrawBorder(dx, dy, dw, dh, 18.0f, 1.0f, 0x33FFFFFF);
    } else {
        RenderModifiers(node, viewport_);
    }

    float pad = 14.0f;
    float cur_x = viewport_.x + pad;
    size_t count = node.GetChildren().size();
    float item_w = (count > 0) ? (viewport_.width - pad * 2.0f - (count - 1) * node.GetSpacing()) / count : 50.0f;

    for (const auto& child : node.GetChildren()) {
        if (!child) continue;
        core::Rect child_vp{cur_x, viewport_.y + 6.0f, item_w, viewport_.height - 12.0f};
        CanvasRenderVisitor child_visitor(fb_, child_vp);
        child->Accept(child_visitor);
        cur_x += item_w + node.GetSpacing();
    }
}

void CanvasRenderVisitor::Visit(scene::AppGroupNode& node) {
    RenderModifiers(node, viewport_);
    for (const auto& child : node.GetChildren()) {
        if (!child) continue;
        CanvasRenderVisitor child_visitor(fb_, viewport_);
        child->Accept(child_visitor);
    }
}

void CanvasRenderVisitor::Visit(scene::ToggleNode& node) {
    int tx = static_cast<int>(viewport_.x);
    int ty = static_cast<int>(viewport_.y + 6);
    int track_w = 46;
    int track_h = 24;

    // Optional leading label
    if (!node.GetLabel().empty()) {
        fb_.DrawTextSimple(tx, ty + 6, node.GetLabel(), 0xFFE0E0E0);
        tx += static_cast<int>(viewport_.width) - track_w;
    }

    // Capsule track
    uint32_t track_col = node.GetState() ? 0xFF34C759 : 0xFF3A3A3C; // Apple Green or Dark Slate
    fb_.DrawRoundedRect(tx, ty, track_w, track_h, 12.0f, track_col);

    // Sliding knob (Circle)
    int knob_x = node.GetState() ? (tx + track_w - 14) : (tx + 14);
    int knob_y = ty + track_h / 2;
    fb_.DrawCircle(knob_x, knob_y, 9, 0xFFFFFFFF);
}

void CanvasRenderVisitor::Visit(scene::TextInputNode& node) {
    int ix = static_cast<int>(viewport_.x);
    int iy = static_cast<int>(viewport_.y + 4);
    int iw = static_cast<int>(viewport_.width);
    int ih = 34;

    // Background & border
    uint32_t bg_col = node.IsFocused() ? 0x662C303E : 0x441F232D;
    uint32_t border_col = node.IsFocused() ? 0xFF007AFF : 0x33FFFFFF;

    fb_.DrawRoundedRect(ix, iy, iw, ih, 8.0f, bg_col);
    fb_.DrawBorder(ix, iy, iw, ih, 8.0f, 1.5f, border_col);

    if (node.GetText().empty()) {
        fb_.DrawTextSimple(ix + 12, iy + 10, node.GetPlaceholder(), 0xFF7E8492);
    } else {
        fb_.DrawTextSimple(ix + 12, iy + 10, node.GetText(), 0xFFFFFFFF);
    }

    if (node.IsFocused()) {
        int cursor_x = ix + 12 + static_cast<int>(node.GetText().size()) * 8;
        fb_.DrawRect(cursor_x, iy + 8, 2, 18, 0xFF007AFF);
    }
}

void CanvasRenderVisitor::Visit(scene::ProgressBarNode& node) {
    int px = static_cast<int>(viewport_.x);
    int py = static_cast<int>(viewport_.y + 14);
    int pw = static_cast<int>(viewport_.width);
    int ph = 6;

    // Track
    fb_.DrawRoundedRect(px, py, pw, ph, 3.0f, 0x44333846);

    // Filled progress
    int fw = static_cast<int>(pw * std::max(0.0, std::min(1.0, node.GetProgress())));
    if (fw > 0) {
        auto c = node.GetTint();
        uint32_t col = (static_cast<uint32_t>(c.a) << 24) | (static_cast<uint32_t>(c.b) << 16) | (static_cast<uint32_t>(c.g) << 8) | c.r;
        fb_.DrawRoundedRect(px, py, fw, ph, 3.0f, col);
    }
}

void CanvasRenderVisitor::Visit(scene::BadgeNode& node) {
    int bx = static_cast<int>(viewport_.x);
    int by = static_cast<int>(viewport_.y + 8);
    int text_w = static_cast<int>(node.GetText().size()) * 8 + 16;
    int bh = 22;

    auto c = node.GetColor();
    uint32_t col = (static_cast<uint32_t>(c.a) << 24) | (static_cast<uint32_t>(c.b) << 16) | (static_cast<uint32_t>(c.g) << 8) | c.r;
    fb_.DrawRoundedRect(bx, by, text_w, bh, 11.0f, col);
    fb_.DrawTextSimple(bx + 8, by + 5, node.GetText(), 0xFFFFFFFF);
}

void CanvasRenderVisitor::Visit(scene::SpacerNode& node) {
    (void)node;
}

} // namespace prism::render
