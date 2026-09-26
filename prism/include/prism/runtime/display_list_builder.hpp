#pragma once
#include "prism/runtime/render_tree.hpp"

namespace prism::runtime {
class DisplayListBuilder {
public:
    static contracts::DisplayList Build(const RenderTree& tree, contracts::WindowId window,
                                        contracts::ResourceId font, std::uint64_t generation);
};
} // namespace prism::runtime
