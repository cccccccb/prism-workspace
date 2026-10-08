#include "prism/runtime/display_list_builder.hpp"
#include "scene_p.hpp"

#include <cmath>
#include <stdexcept>

namespace prism::runtime {
bool Scene::CanCaptureTaskPaint(contracts::NodeId root) const
{
    if (!root || !render_tree_ || Has(dirty_, Dirty::Layout) || Has(dirty_, Dirty::Paint) ||
        HasPopupSurfaceAdoption()) {
        return false;
    }

    const auto *target = Find(root);
    if (!target || !IsVisible(*target)) {
        return false;
    }
    // Backdrop changes are Composite-only and need not rebuild the render tree.
    // Check current eligibility as well as the resolved source's eligibility.
    for (const auto *ancestor = target; ancestor; ancestor = ancestor->parent) {
        if (ancestor->style.backdrop_blur > 0) {
            return false;
        }
    }
    std::vector<const Node *> pending{target};
    while (!pending.empty()) {
        const auto *node = pending.back();
        pending.pop_back();
        if (!IsVisible(*node) || node->bounds.width <= 0 || node->bounds.height <= 0) {
            continue;
        }
        if (node->style.backdrop_blur > 0) {
            return false;
        }
        for (const auto &child : node->children) {
            pending.push_back(child.get());
        }
    }

    return true;
}

std::optional<contracts::DisplayList> Scene::CaptureTaskPaint(contracts::NodeId root) const
{
    if (!CanCaptureTaskPaint(root)) {
        return std::nullopt;
    }

    return DisplayListBuilder::BuildSubtree(*render_tree_, root, popup_snapshot_window_, font_,
                                            generation_);
}

std::optional<contracts::DisplayList> Scene::CaptureTaskOpacityFrame(contracts::NodeId root,
                                                                     double opacity) const
{
    if (!std::isfinite(opacity) || opacity < 0 || opacity > 1) {
        throw std::invalid_argument("Task opacity must be finite and in [0,1]");
    }
    if (!CanCaptureTaskPaint(root)) {
        return std::nullopt;
    }

    return DisplayListBuilder::BuildWithSubtreeOpacity(*render_tree_, root, opacity,
                                                       popup_snapshot_window_, font_, generation_);
}
} // namespace prism::runtime
