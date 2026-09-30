#pragma once

#include "prism/contracts/types.hpp"
#include "prism/runtime/gesture_spec.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace prism::runtime {

// Published only through shared_ptr<const InputSnapshot>. Geometry is in window
// logical coordinates. Visual subtrees are decorative and have no input nodes;
// interactive transforms will require inverse transforms in this contract.
struct InputSnapshotNode {
    contracts::NodeId id{};
    contracts::NodeId parent{};
    std::vector<contracts::NodeId> children;
    contracts::LogicalRect bounds{};
    double radius{};
    bool clip{};
    bool visible{}; // Includes ancestor visibility.
    bool enabled{}; // Includes ancestor availability.
    bool interactive{};
    std::string action;
    std::optional<GestureSpec> gesture;

    bool operator==(const InputSnapshotNode &) const = default;
};

struct InputSnapshot {
    std::uint64_t scene{};   // Process-local Scene identity; node indices alone are not sufficient.
    std::uint64_t version{}; // Nonzero and monotonic within one Scene.
    contracts::NodeId root{};
    contracts::LogicalSize viewport{};
    std::vector<InputSnapshotNode> nodes;

    const InputSnapshotNode *Find(contracts::NodeId id) const noexcept
    {
        if (!id || id.index >= nodes.size() || nodes[id.index].id != id) {
            return nullptr;
        }
        return &nodes[id.index];
    }
};

} // namespace prism::runtime
