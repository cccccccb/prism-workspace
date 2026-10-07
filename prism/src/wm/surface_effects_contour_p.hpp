#pragma once

#include "prism/contracts/contour.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace prism::wm::effect_detail {

inline constexpr std::size_t ContourMaskPixelLimit = 16u * 1024u * 1024u;

// Input topology was validated at transport installation. Presentation may
// produce non-quantized coordinates; this pass never re-quantizes them. The
// origin locates ContourBounds' top-left within the target. Output rows are
// logical top-to-bottom, with four vertical samples and exact horizontal area.
// Invalid extents/coordinates/budgets throw std::invalid_argument; allocation
// failure propagates to the GPU installation boundary for visible failure.
std::vector<std::uint8_t> RasterizeContourCoverage(const contracts::Contour &contour, int width,
                                                   int height, double origin_x, double origin_y);

} // namespace prism::wm::effect_detail
