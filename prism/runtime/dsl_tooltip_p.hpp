#pragma once

#include "prism/runtime/prepared_component.hpp"

namespace prism::runtime {
void ValidatePreparedTooltipTree(const PreparedNode &, const ComponentSource &,
                                 bool await_regions = false);
} // namespace prism::runtime
