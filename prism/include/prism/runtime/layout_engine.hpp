#pragma once
#include "prism/runtime/scene_snapshot.hpp"

namespace prism::runtime {
class LayoutEngine {
public:
    static void Compute(SceneSnapshot &snapshot, contracts::LogicalSize viewport,
                        const ShapeText &shaper);
    static void ComputeSubtree(SceneSnapshot &snapshot, contracts::NodeId root,
                               contracts::LogicalRect bounds, const ShapeText &shaper);
};
} // namespace prism::runtime
