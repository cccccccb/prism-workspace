#pragma once
#include "include/core/SkRect.h"
#include "prism/contracts/display_list.hpp"
#include <optional>
#include <vector>

namespace prism::render_skia::detail {
class ResourceTable;

// One local-space ink hint per PushOpacity, in command order. An empty vector
// means every group is opaque/transparent and no intermediate layer is needed.
std::vector<std::optional<SkRect>> OpacityLayerHints(const contracts::DisplayList &list,
                                                     const ResourceTable &resources);
} // namespace prism::render_skia::detail
