#pragma once

#include "prism/runtime/prepared_component.hpp"

namespace prism::runtime {

struct PreparedComponent::Data {
    ComponentSource source;
    PreparedNode root;
    std::vector<PreparedImage> images;
    std::size_t source_bytes{0};
    std::size_t node_count{0};
};

} // namespace prism::runtime
