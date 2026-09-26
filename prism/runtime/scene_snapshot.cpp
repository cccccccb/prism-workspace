#include "prism/runtime/scene_snapshot.hpp"
#include <stdexcept>
#include <utility>

namespace prism::runtime {
const SnapshotNode& SceneSnapshot::Get(contracts::NodeId id) const {
    if (!id || id.index >= nodes.size() || nodes[id.index].id != id)
        throw std::out_of_range("Invalid snapshot node");
    return nodes[id.index];
}
SnapshotNode& SceneSnapshot::Get(contracts::NodeId id) {
    return const_cast<SnapshotNode&>(std::as_const(*this).Get(id));
}
} // namespace prism::runtime
