#pragma once
#include "prism/contracts/contour.hpp"
#include "prism/contracts/panel_contour.hpp"
#include "prism/runtime/presentation.hpp"
#include "prism/runtime/scene.hpp"
#include <memory>

namespace prism::runtime {

// Immutable input to RenderTreeBuilder after LayoutEngine has resolved geometry.
// Indexed by NodeId.index; no pointers to live Scene nodes escape this value.
struct SnapshotNode {
    contracts::NodeId id{};
    Kind kind{Kind::Box};
    contracts::NodeId popup_anchor;
    contracts::NodeId tooltip_anchor;
    contracts::NodeId parent;
    Style style{};
    VisualPresentation presentation{};
    std::string text;
    std::string icon;
    double value{0};
    double scroll_offset{}, scroll_content_height{};
    bool checked{false};
    InteractionState interaction{};
    contracts::ResourceId image{};
    contracts::LogicalSize intrinsic_size{};
    bool image_ready{false};
    contracts::LogicalRect bounds{};
    std::shared_ptr<const contracts::Contour> contour_source;
    std::shared_ptr<const contracts::Contour> contour;
    std::optional<contracts::PanelContourSpec> contour_spec;
    std::optional<contracts::PanelContourRequest> contour_request;
    std::shared_ptr<const contracts::Contour> contour_prepared;
    std::optional<PopupPlacement> popup_placement;
    ShapedText shaped{};
    std::vector<contracts::LogicalRect> text_selection;
    contracts::LogicalRect text_caret{};
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
