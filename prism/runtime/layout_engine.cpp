#include "prism/runtime/layout_engine.hpp"
#include <algorithm>
#include <stdexcept>

namespace prism::runtime {
namespace {
void Place(SceneSnapshot& snapshot, contracts::NodeId id, contracts::LogicalRect bounds,
           const ShapeText& shaper) {
    auto& node = snapshot.Get(id);
    node.bounds = bounds;
    if (node.kind == Kind::Text) {
        node.shaped = shaper(node.text, node.style.font_size);
        return;
    }
    if (node.kind == Kind::Image) return;
    const double pad = std::max(0.0, node.style.padding);
    const double x = bounds.x + pad, y = bounds.y + pad;
    const double width = std::max(0.0, bounds.width - 2 * pad);
    const double height = std::max(0.0, bounds.height - 2 * pad);
    if (node.kind == Kind::Box) {
        for (auto child_id : node.children) {
            const auto& child = snapshot.Get(child_id);
            const auto& style = child.style;
            const double w = style.width > 0 ? std::min(style.width, width) : width;
            const double h = style.height > 0 ? std::min(style.height, height)
                : child.kind == Kind::Text ? std::min(style.font_size * 1.4, height) : height;
            Place(snapshot, child_id, {x, y, w, h}, shaper);
        }
        return;
    }
    const bool row = node.kind == Kind::Row;
    const double main = row ? width : height;
    const double cross = row ? height : width;
    const double gap = std::max(0.0, node.style.spacing);
    const double total_gap = gap * (node.children.empty() ? 0 : node.children.size() - 1);
    double fixed = 0;
    std::size_t flexible = 0;
    for (auto child_id : node.children) {
        const auto& child = snapshot.Get(child_id);
        const double explicit_size = row ? child.style.width : child.style.height;
        if (explicit_size > 0) fixed += explicit_size;
        else if (!row && child.kind == Kind::Text) fixed += child.style.font_size * 1.4;
        else if (child.kind == Kind::Image && child.image_ready)
            fixed += row ? child.intrinsic_size.width : child.intrinsic_size.height;
        else ++flexible;
    }
    const double remaining = std::max(0.0, main - total_gap - fixed);
    const double flex_size = flexible ? remaining / flexible : 0;
    double cursor = row ? x : y;
    for (auto child_id : node.children) {
        const auto& child = snapshot.Get(child_id);
        const double explicit_size = row ? child.style.width : child.style.height;
        const double intrinsic = !row && child.kind == Kind::Text ? child.style.font_size * 1.4
            : child.kind == Kind::Image && child.image_ready
                ? (row ? child.intrinsic_size.width : child.intrinsic_size.height) : flex_size;
        const double length = std::max(0.0, std::min(explicit_size > 0 ? explicit_size : intrinsic,
            std::max(0.0, (row ? x + width : y + height) - cursor)));
        const double cross_explicit = row ? child.style.height : child.style.width;
        const double cross_intrinsic = child.kind == Kind::Image && child.image_ready
            ? (row ? child.intrinsic_size.height : child.intrinsic_size.width) : cross;
        const double other = std::min(cross_explicit > 0 ? cross_explicit : cross_intrinsic, cross);
        Place(snapshot, child_id, row ? contracts::LogicalRect{cursor, y, length, other}
                                     : contracts::LogicalRect{x, cursor, other, length}, shaper);
        cursor += length + gap;
    }
}
} // namespace

void LayoutEngine::Compute(SceneSnapshot& snapshot, contracts::LogicalSize viewport,
                           const ShapeText& shaper) {
    if (!snapshot.root || !shaper) throw std::invalid_argument("Layout needs a root and text shaper");
    Place(snapshot, snapshot.root, {0, 0, viewport.width, viewport.height}, shaper);
}
} // namespace prism::runtime
