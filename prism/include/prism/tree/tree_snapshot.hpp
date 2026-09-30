#pragma once

#include "prism/tree/tree_node.hpp"

namespace prism::tree {

// Tree identities are process-unique and are scoped by WM session on the wire.
// These values contain no mutable tree or Window pointers.
struct TreeNodeSnapshot {
    std::uint64_t id{0};
    std::uint64_t parent{0};
    std::uint64_t workspace{0};
    std::uint64_t active_child{0};
    NodeType type{NodeType::Container};
    LayoutMode layout{LayoutMode::None};
    core::Rect bounds{};
    double width_fraction{0};
    double height_fraction{0};
    std::vector<std::uint64_t> children;
};

struct TreeWorkspaceSnapshot {
    std::uint64_t id{0};
    std::uint64_t root{0};
    std::string name;
    bool active{false};
};

struct TreeBoundarySnapshot {
    std::uint64_t id{0};
    std::uint64_t parent{0};
    std::uint64_t workspace{0};
    std::uint64_t before{0};
    std::uint64_t after{0};
    LayoutMode axis{LayoutMode::None};
    core::Rect bounds{};
};

struct TreeSnapshot {
    std::uint64_t topology_revision{0};
    std::uint64_t layout_revision{0};
    std::uint64_t focus_revision{0};
    std::uint64_t active_workspace{0};
    std::uint64_t focused_node{0};
    std::vector<TreeWorkspaceSnapshot> workspaces;
    std::vector<TreeNodeSnapshot> nodes;
    std::vector<TreeBoundarySnapshot> boundaries;
};

} // namespace prism::tree
