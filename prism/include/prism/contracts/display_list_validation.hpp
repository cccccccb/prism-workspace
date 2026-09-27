#pragma once

#include "prism/contracts/display_list.hpp"

namespace prism::contracts {
// Backend-independent numeric, size and clip/transform structure validation.
// Throws std::invalid_argument with a structural diagnostic on invalid input.
// Font/image handle validity and registration belong to the resource owner.
void ValidateDisplayList(const DisplayList &list);
} // namespace prism::contracts
