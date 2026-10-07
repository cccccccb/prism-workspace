#pragma once

#include "prism/contracts/contour.hpp"

#include <optional>

namespace prism::contracts {

enum class PanelAttachmentEdge { None, Top, Bottom };
enum class PanelNeckShape { SoftTab, RoundedTriangle };

struct PanelContourSpec {
    double radius{};
    double neck_width{};
    double neck_height{};
    PanelNeckShape neck_shape{PanelNeckShape::SoftTab};
    bool operator==(const PanelContourSpec &) const noexcept = default;
};

struct PanelContourRequest {
    LogicalSize body;
    PanelContourSpec spec;
    PanelAttachmentEdge edge{PanelAttachmentEdge::None};
    double center{};
    bool operator==(const PanelContourRequest &) const noexcept = default;
};

// Pure geometry boundaries. Invalid finite values/ranges throw invalid_argument.
// Radius and neck width are 0..256; neck height is 0..48 logical pixels.
// Unknown neck shapes are rejected even for detached or disabled necks.
void ValidatePanelContourSpec(const PanelContourSpec &spec);
double PanelNeckHeight(const PanelContourSpec &spec);

// The width is the full shoulder footprint. The complete footprint must fit
// between the normalized body corners, and its center must lie in the anchor's
// horizontal interval. Disabled necks, no fit or no overlap return nullopt.
std::optional<double> PanelAttachmentCenter(LogicalSize body, const PanelContourSpec &spec,
                                            double preferred, double anchor_left,
                                            double anchor_right);

// Body remains (0,0,width,height). Top necks extend into negative Y; Bottom is
// mirrored before the common flatten/quantize pass. SoftTab preserves the
// horizontal tip. RoundedTriangle uses two tangent-continuous cubic curves per
// side, with broad shoulders, no straight diagonal and one rounded apex.
// Radius controls the apex softness within the same shoulder width/neck height.
// Radius zero uses only lines and retains the selected rectangular/triangle tip.
// The final simple polygon obeys the ordinary contour topology/extent budget.
Contour PreparePanelContour(const PanelContourRequest &request);

} // namespace prism::contracts
