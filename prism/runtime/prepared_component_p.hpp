#pragma once

#include "prism/runtime/prepared_component.hpp"
#include <utility>

namespace prism::runtime {

struct PreparedComponent::Data {
    ComponentSource source;
    PreparedNode root;
    std::vector<PreparedImage> images;
    std::size_t source_bytes{0};
    std::size_t node_count{0};
};

struct SyntaxNode;

// Generic composition metadata supplied after the caller validates syntax and
// paths. Ordinary visual preparation supplies no pending region placeholders.
struct PreparedRegionPlaceholder {
    std::string_view region;
    std::span<const std::size_t> node_path;
};

// Internal access keeps the public prepared result immutable and AST-free.
struct PreparedComponentAccess {
    static PreparedComponent Make(ComponentSource source, PreparedNode root,
                                  std::vector<PreparedImage> images, std::size_t bytes,
                                  std::size_t nodes)
    {
        auto data = std::make_shared<PreparedComponent::Data>();
        data->source = std::move(source);
        data->root = std::move(root);
        data->images = std::move(images);
        data->source_bytes = bytes;
        data->node_count = nodes;
        return PreparedComponent(std::move(data));
    }
};

PreparedComponent
PrepareVisualSyntax(const SyntaxNode &, ComponentSource, std::size_t bytes,
                    std::span<const PreparedRegionPlaceholder> pending_regions = {});

} // namespace prism::runtime
