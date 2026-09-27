#include "prism/runtime/ui_load.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <memory>
#include <set>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>

namespace {
using prism::runtime::UiLoadId;
using prism::runtime::UiLoadState;

void CheckGenerations()
{
    UiLoadState state;
    assert(!state.Current({}));
    state.Cancel();

    const auto first = state.Begin();
    assert(first.owner != 0 && first.generation == 1);
    assert(state.Current(first));
    assert(state.Current(first)); // Freshness checks do not consume a result.
    assert(!state.Current({first.owner, 0}));
    assert(!state.Current({0, first.generation}));

    const auto next = state.Begin();
    assert(next.owner == first.owner && next.generation == first.generation + 1);
    assert(state.Current(next));
    assert(!state.Current(first));
    assert(!state.Current(first)); // Repeated late completion remains stale.

    state.Cancel();
    state.Cancel();
    assert(!state.Current(next));
    const auto resumed = state.Begin();
    assert(resumed.owner == next.owner && resumed.generation == next.generation + 1);
    assert(state.Current(resumed));
    assert(!state.Current(next));
    assert(!state.Current(first));
}

void CheckOwners()
{
    UiLoadState first;
    UiLoadState second;
    const auto a = first.Begin();
    const auto b = second.Begin();
    assert(a.owner != b.owner && a.generation == b.generation);
    assert(!first.Current(b));
    assert(!second.Current(a));
    assert(first.Current(a) && second.Current(b));

    first.Cancel();
    assert(second.Current(b));
}

void CheckReusedStorage()
{
    alignas(UiLoadState) std::byte storage[sizeof(UiLoadState)];
    auto *first = std::construct_at(reinterpret_cast<UiLoadState *>(storage));
    const auto stale = first->Begin();
    std::destroy_at(first);

    auto *second = std::construct_at(reinterpret_cast<UiLoadState *>(storage));
    const auto fresh = second->Begin();
    assert(fresh.owner > stale.owner && fresh.generation == stale.generation);
    assert(!second->Current(stale));
    assert(second->Current(fresh));
    std::destroy_at(second);
}

struct AllocateOwners {
    std::span<UiLoadId> tokens;

    void operator()() const
    {
        for (auto &token : tokens) {
            UiLoadState state;
            token = state.Begin();
            assert(state.Current(token));
        }
    }
};

void CheckConcurrentOwners()
{
    std::array<std::array<UiLoadId, 64>, 4> tokens{};
    std::array<std::thread, 4> threads;
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i] = std::thread(AllocateOwners{tokens[i]});
    }
    for (auto &thread : threads) {
        thread.join();
    }

    std::set<std::uint64_t> owners;
    for (const auto &group : tokens) {
        for (const auto token : group) {
            assert(token.owner != 0 && token.generation == 1);
            assert(owners.insert(token.owner).second);
        }
    }
}
} // namespace

int main()
{
    static_assert(std::is_trivially_copyable_v<UiLoadId>);
    static_assert(!std::is_copy_constructible_v<UiLoadState>);
    static_assert(!std::is_move_constructible_v<UiLoadState>);
    static_assert(noexcept(std::declval<const UiLoadState &>().Current({})));
    static_assert(noexcept(std::declval<UiLoadState &>().Cancel()));

    CheckGenerations();
    CheckOwners();
    CheckReusedStorage();
    CheckConcurrentOwners();
}
