#pragma once
#include "prism/runtime/scene_snapshot.hpp"
#include <variant>

namespace prism::runtime {
struct RectVisual {
    contracts::Color color;
};

struct RoundedRectVisual {
    double radius;
    contracts::Color color;
};

struct ImageVisual {
    contracts::ResourceId image;
    contracts::ImageFit fit;
};

struct TextVisual {
    ShapedText shaped;
    double font_size;
    contracts::Color color;
};

struct BorderVisual {
    double radius;
    double width;
    contracts::Color color;
};

struct ShadowVisual {
    double radius;
    double blur;
    double offset_y;
    contracts::Color color;
    bool inset;
};

struct IconVisual {
    contracts::VectorIcon icon;
    contracts::Color color;
    double padding;
};

struct ProgressVisual {
    double value;
    double radius;
    contracts::Color color;
};

struct ToggleVisual {
    bool checked;
    contracts::Color color;
    contracts::ThemeControls controls;
};

using Visual = std::variant<RectVisual, RoundedRectVisual, ImageVisual, TextVisual, BorderVisual,
                            ShadowVisual, IconVisual, ProgressVisual, ToggleVisual>;

struct RenderNode {
    contracts::NodeId id{};
    contracts::LogicalRect bounds{};
    bool visible{true};
    bool clip{false};
    double clip_radius{0};
    bool presentation_scope{false};
    VisualPresentation presentation{};
    std::vector<Visual> visuals;
    std::vector<contracts::NodeId> children;
    std::uint64_t source_revision{0};
    std::uint64_t render_generation{0};
};

struct RenderTree {
    contracts::NodeId root{};
    std::vector<RenderNode> nodes;
    const RenderNode &Get(contracts::NodeId id) const;
};

class RenderTreeBuilder {
public:
    static RenderTree Build(const SceneSnapshot &snapshot, const RenderTree *previous = nullptr);
};
} // namespace prism::runtime
