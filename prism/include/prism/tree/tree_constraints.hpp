#pragma once

#include "prism/tree/tree_node.hpp"

namespace prism::tree {

struct TreeMinimumSize {
    double width{0};
    double height{0};
};

struct BoundaryRange {
    std::uint64_t boundary{0};
    std::uint64_t parent{0};
    std::uint64_t workspace{0};
    std::uint64_t before{0};
    std::uint64_t after{0};
    LayoutMode axis{LayoutMode::None};

    // Gap centre, in the parent's local logical coordinates along the split axis.
    double position{0};
    double minimum{0};
    double maximum{0};
    double pair_fraction{0};
    bool feasible{false};
};

struct BoundaryFractions {
    std::uint64_t boundary{0};
    std::uint64_t parent{0};
    std::uint64_t before{0};
    std::uint64_t after{0};
    std::uint64_t topology_revision{0};
    LayoutMode axis{LayoutMode::None};
    double before_fraction{0};
    double after_fraction{0};
};

} // namespace prism::tree
