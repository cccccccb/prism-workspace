#pragma once
#include "prism/runtime/render_tree.hpp"
#include <optional>

namespace prism::runtime {
class DisplayListBuilder {
public:
    static contracts::DisplayList Build(const RenderTree &tree, contracts::WindowId window,
                                        contracts::ResourceId font, std::uint64_t generation);
    // Copies only the resolved subtree and its ancestor drawing context. Image
    // and backdrop dependencies require resource leases and are not exportable.
    static std::optional<contracts::DisplayList>
    BuildSubtree(const RenderTree &tree, contracts::NodeId root, contracts::WindowId window,
                 contracts::ResourceId font, std::uint64_t generation);
    // Replays the full resolved tree in its original order, with one group
    // opacity at the selected subtree. Zero omits it; one preserves Build's
    // exact commands. The same resource eligibility as BuildSubtree applies.
    // Invalid opacity throws; unavailable or unsupported roots return nullopt.
    static std::optional<contracts::DisplayList>
    BuildWithSubtreeOpacity(const RenderTree &tree, contracts::NodeId root, double opacity,
                            contracts::WindowId window, contracts::ResourceId font,
                            std::uint64_t generation);
};
} // namespace prism::runtime
