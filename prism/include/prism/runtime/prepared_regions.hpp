#pragma once

#include "prism/runtime/prepared_component.hpp"
#include <string>

namespace prism::runtime {
struct PreparedRegion {
    std::string region;
    PreparedComponent prepared;
};
} // namespace prism::runtime
