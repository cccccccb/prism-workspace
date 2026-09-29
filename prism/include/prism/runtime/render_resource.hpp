#pragma once

#include "prism/contracts/types.hpp"
#include <cstdint>

namespace prism::runtime {

// Assigned by the UI resource source, independently of either Skia table.
// An ID may never be interpreted as proof that a particular version is live.
struct ImageVersion {
    contracts::ResourceId id{};
    std::uint64_t generation{};

    bool operator==(const ImageVersion &) const = default;
};

} // namespace prism::runtime
