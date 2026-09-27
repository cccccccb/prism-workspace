#include "prism/runtime/ui_load.hpp"
#include <atomic>
#include <limits>
#include <stdexcept>

namespace prism::runtime {
namespace {
std::uint64_t AllocateUiOwner()
{
    // Independent UI owners may be constructed on different owner threads.
    // Only uniqueness is shared; this counter does not publish UI state.
    static std::atomic<std::uint64_t> last_owner{};
    auto previous = last_owner.load(std::memory_order_relaxed);
    for (;;) {
        if (previous == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("UI load owner IDs exhausted");
        }

        const auto next = previous + 1;
        if (last_owner.compare_exchange_weak(previous, next, std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
            return next;
        }
    }
}
} // namespace

UiLoadState::UiLoadState() : owner_(AllocateUiOwner())
{
}

UiLoadId UiLoadState::Begin()
{
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("UI load generations exhausted");
    }

    ++generation_;
    active_ = true;
    return {owner_, generation_};
}

bool UiLoadState::Current(UiLoadId id) const noexcept
{
    return active_ && id.owner != 0 && id.generation != 0 && id.owner == owner_ &&
           id.generation == generation_;
}

void UiLoadState::Cancel() noexcept
{
    active_ = false;
}
} // namespace prism::runtime
