#pragma once

#include <cstdint>

namespace prism::contracts {

// Issued by the context/WSI owner, never derived from recyclable native handles.
// Context and surface lifetime IDs are nonzero and are not reused. A live
// surface starts at generation 1 and advances it when its dimensions change.
struct GpuTargetIdentity {
    std::uint64_t context_lifetime_id{};
    std::uint64_t surface_lifetime_id{};
    std::uint64_t resize_generation{};

    explicit operator bool() const noexcept
    {
        return context_lifetime_id && surface_lifetime_id && resize_generation;
    }

    bool operator==(const GpuTargetIdentity &) const = default;
};

} // namespace prism::contracts
