#pragma once

namespace prism::tree {
// Layout data has no dependency on decoration/rendering or any theme syntax.
struct TreeLayoutConfig {
    int inner_gap{};
    int outer_gap{};
    bool smart_gaps{};
    float header_height{};
};
} // namespace prism::tree
