#pragma once

namespace prism::runtime {
// Local visual-only transform. Layout and input geometry remain unchanged.
// The origin is normalized to the arranged node bounds; translation is applied
// after scaling around that origin. Opacity composites the complete subtree.
struct VisualPresentation {
    double translate_x{0};
    double translate_y{0};
    double scale_x{1};
    double scale_y{1};
    double origin_x{0.5};
    double origin_y{0.5};
    double opacity{1};
    bool operator==(const VisualPresentation &) const noexcept = default;
};
} // namespace prism::runtime
