#pragma once

#include "prism/contracts/panel_contour.hpp"

#include <string>
#include <variant>

namespace prism::runtime {

struct ContourThemeNumber {
    std::string name;
    bool operator==(const ContourThemeNumber &) const = default;
};

using ContourNumber = std::variant<double, ContourThemeNumber>;

enum class ContourFallback { Detached };

// Owning preparation metadata. Theme references resolve on the Scene owner;
// final placement supplies the geometry used to prepare the canonical contour.
struct AttachedPanelRecipe {
    ContourNumber radius;
    ContourNumber neck_width;
    ContourNumber neck_height;
    ContourFallback fallback{ContourFallback::Detached};
    contracts::PanelNeckShape neck_shape{contracts::PanelNeckShape::SoftTab};
    bool operator==(const AttachedPanelRecipe &) const = default;
};

} // namespace prism::runtime
