#pragma once
#include "prism/runtime/scene.hpp"

namespace prism::runtime {

// Immutable input to RenderTreeBuilder after LayoutEngine has resolved geometry.
// Indexed by NodeId.index; no pointers to live Scene nodes escape this value.
struct SnapshotNode {
    contracts::NodeId id{};
    Kind kind{Kind::Box};
    Style style{};
    std::string text;
    std::string icon;
    double value{0};
    bool checked{false};
    InteractionState interaction{};
    contracts::ResourceId image{};
    contracts::LogicalSize intrinsic_size{};
    bool image_ready{false};
    contracts::LogicalRect bounds{};
    ShapedText shaped{};
    std::vector<contracts::NodeId> children;
    std::uint64_t revision{0};
};

struct SceneSnapshot {
    contracts::NodeId root{};
    std::vector<SnapshotNode> nodes;
    contracts::ThemeControls controls{{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const SnapshotNode &Get(contracts::NodeId id) const;
    SnapshotNode &Get(contracts::NodeId id);
};
} // namespace prism::runtime
