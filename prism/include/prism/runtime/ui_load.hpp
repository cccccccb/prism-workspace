#pragma once
#include <cstdint>

namespace prism::runtime {
// A value carried by immutable preparation results; zero fields are invalid.
// The owner identifies one state lifetime, never an object address or surface.
struct UiLoadId {
    std::uint64_t owner{};
    std::uint64_t generation{};
    bool operator==(const UiLoadId &) const = default;
};

// Construct and call only on the UI owner's thread. Tokens may cross threads,
// but this state must not be read or mutated by preparation workers.
class UiLoadState {
public:
    // Owner allocation fails with overflow_error after all nonzero IDs are used.
    UiLoadState();
    UiLoadState(const UiLoadState &) = delete;
    UiLoadState &operator=(const UiLoadState &) = delete;
    UiLoadState(UiLoadState &&) = delete;
    UiLoadState &operator=(UiLoadState &&) = delete;

    // Starts a new load and invalidates every preceding generation. Exhausted
    // generation space throws overflow_error, never wraps or reuses an ID.
    // A failed Begin leaves the previous current state unchanged.
    UiLoadId Begin();
    // Checks freshness without consuming the token; one load may return several
    // immutable results. The caller separately decides when a result is applied.
    bool Current(UiLoadId) const noexcept;
    void Cancel() noexcept;

private:
    const std::uint64_t owner_;
    std::uint64_t generation_{};
    bool active_{};
};
} // namespace prism::runtime
