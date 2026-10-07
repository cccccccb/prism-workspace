#pragma once

#include <cstdint>

namespace prism::runtime {

enum class OwnerModalCloseReason { Escape, Unavailable, Ended };

struct OwnerModalClosure {
    std::uint64_t token{};
    OwnerModalCloseReason reason{OwnerModalCloseReason::Ended};

    bool operator==(const OwnerModalClosure &) const = default;
};

} // namespace prism::runtime
