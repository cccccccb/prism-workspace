#pragma once
#include "prism/runtime/scene_snapshot.hpp"
#include <variant>

namespace prism::runtime {
struct RectVisual { contracts::Color color; };
struct RoundedRectVisual { double radius; contracts::Color color; };
struct ImageVisual { contracts::ResourceId image; };
struct TextVisual { ShapedText shaped; double font_size; contracts::Color color; };
using Visual = std::variant<RectVisual, RoundedRectVisual, ImageVisual, TextVisual>;

struct RenderNode {
    contracts::NodeId id{};
    contracts::LogicalRect bounds{};
    bool clip{false};
    std::vector<Visual> visuals;
    std::vector<contracts::NodeId> children;
    std::uint64_t source_revision{0};
    std::uint64_t render_generation{0};
};
struct RenderTree {
    contracts::NodeId root{};
    std::vector<RenderNode> nodes;
    const RenderNode& Get(contracts::NodeId id) const;
};
class RenderTreeBuilder {
public:
    static RenderTree Build(const SceneSnapshot& snapshot, const RenderTree* previous = nullptr);
};
} // namespace prism::runtime
