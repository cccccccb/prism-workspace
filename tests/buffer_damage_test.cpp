#include "prism/runtime/buffer_damage.hpp"
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

// Commit runs after successful WSI submission. Count allocations in that tiny
// boundary rather than inferring safety from noexcept alone.
namespace {
std::atomic<bool> track_allocations{};
std::atomic<std::size_t> allocations{};
} // namespace

void *operator new(std::size_t size)
{
    if (track_allocations.load(std::memory_order_relaxed)) {
        allocations.fetch_add(1, std::memory_order_relaxed);
    }
    if (void *pointer = std::malloc(size ? size : 1)) {
        return pointer;
    }
    throw std::bad_alloc();
}

void *operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void *pointer) noexcept
{
    std::free(pointer);
}

void operator delete[](void *pointer) noexcept
{
    std::free(pointer);
}

void operator delete(void *pointer, std::size_t) noexcept
{
    std::free(pointer);
}

void operator delete[](void *pointer, std::size_t) noexcept
{
    std::free(pointer);
}

namespace {
using namespace prism;
using contracts::BufferSize;
using contracts::DamageRect;
using contracts::DamageRegion;
using runtime::BufferDamageHistory;
using runtime::BufferDamagePlan;
constexpr BufferSize size{100, 100};

DamageRegion Delta(int x, int y, int width = 5, int height = 5)
{
    return {false, {{x, y, width, height}}};
}

bool Covers(const DamageRegion &region, int x, int y)
{
    if (region.full) {
        return true;
    }
    for (const auto &rect : region.rects) {
        if (x >= rect.x && y >= rect.y && std::int64_t(x) < std::int64_t(rect.x) + rect.width &&
            std::int64_t(y) < std::int64_t(rect.y) + rect.height) {
            return true;
        }
    }
    return false;
}

void SameCoverage(const DamageRegion &a, const DamageRegion &b, BufferSize target = size)
{
    for (unsigned y = 0; y < target.height; ++y) {
        for (unsigned x = 0; x < target.width; ++x) {
            assert(Covers(a, x, y) == Covers(b, x, y));
        }
    }
}

void CommitWithoutAllocation(BufferDamageHistory &history, BufferDamagePlan &&plan)
{
    static_assert(noexcept(history.Commit(std::move(plan))));
    allocations = 0;
    track_allocations = true;
    const bool committed = history.Commit(std::move(plan));
    track_allocations = false;
    assert(committed && allocations == 0);
}
} // namespace

int main()
{
    using namespace prism;
    const DamageRegion clipped{false, {{-5, -10, 10, 15}, {500, 500, 10, 10}, {1, 1, 0, 9}}};
    const auto normalized = runtime::NormalizeDamage(clipped, size);
    assert(normalized == Delta(0, 0, 5, 5));
    assert(runtime::DamageArea(clipped, size) == 25);
    const DamageRegion overlapping{false, {{1, 1, 10, 10}, {6, 6, 10, 10}, {1, 1, 10, 10}}};
    const auto unioned = runtime::NormalizeDamage(overlapping, size);
    assert(!unioned.full && runtime::DamageArea(unioned, size) == 175);
    assert(runtime::DamageArea(overlapping, size) == 175);
    SameCoverage(overlapping, unioned); // Preserve the union, not its bounding box.
    assert(!Covers(unioned, 15, 1) && !Covers(unioned, 1, 15));
    const DamageRegion adjacent{false, {{1, 1, 5, 5}, {6, 1, 5, 5}}};
    assert(runtime::NormalizeDamage(adjacent, size) == Delta(1, 1, 10, 5));
    assert(runtime::NormalizeDamage({true, {{1, 1, 2, 2}}}, size) == DamageRegion::Full());
    assert(runtime::NormalizeDamage(Delta(INT32_MAX - 5, 0, 100, 10), size).rects.empty());
    assert(runtime::NormalizeDamage(Delta(0, 0, -1, 1), size).full);
    assert(runtime::DamageArea(DamageRegion::Full(), size) == 10000);
    const auto a = Delta(10, 10), b = Delta(30, 10), c = Delta(50, 10);
    const auto disjoint = runtime::UnionDamage(a, b, size);
    assert(!disjoint.full && disjoint.rects.size() == 2 &&
           runtime::DamageArea(disjoint, size) == 50);
    const DamageRegion regions[]{a, b, c};
    assert(runtime::DamageArea(runtime::UnionDamage(regions, size), size) == 75);
    DamageRegion fragments;
    for (int i = 0; i < 17; ++i) {
        fragments.rects.push_back({2 * i, 40, 1, 1});
    }
    assert(runtime::NormalizeDamage(fragments, size).full);
    const auto increased = runtime::NormalizeDamage(fragments, size, {32, 1});
    assert(!increased.full && increased.rects.size() == 17 &&
           runtime::DamageArea(increased, size) == 17);
    assert(runtime::NormalizeDamage(Delta(0, 0, 76, 100), size).full);
    assert(!runtime::NormalizeDamage(Delta(0, 0, 75, 100), size).full);
    assert(runtime::DamageArea(Delta(0, 0, 76, 100), size) ==
           7600); // Area does not apply policy fallback.
    const contracts::LogicalRect fractional[]{{1.25, 2.2, .5, 1.1}};
    assert(runtime::DamageFromLogicalBounds(fractional, size, 2) == Delta(2, 4, 2, 3));
    const contracts::LogicalRect outside[]{{-.2, -.6, 1.3, 1.8}};
    assert(runtime::DamageFromLogicalBounds(outside, size) == Delta(0, 0, 2, 2));
    const contracts::LogicalRect invalid[]{{std::numeric_limits<double>::quiet_NaN(), 0, 1, 1}};
    assert(runtime::DamageFromLogicalBounds(invalid, size).full);
    assert(runtime::NormalizeDamage(DamageRegion::Full(), {0, 100}).rects.empty());

    BufferDamageHistory rotating(2);
    rotating.Reset(size);
    auto first = rotating.Plan(DamageRegion::Full(), 0);
    assert(first.content_damage.full && first.repair_damage.full);
    CommitWithoutAllocation(rotating, std::move(first));
    auto second = rotating.Plan(a, 1);
    SameCoverage(second.content_damage, a);
    SameCoverage(second.repair_damage, a);
    CommitWithoutAllocation(rotating, std::move(second));
    auto third = rotating.Plan(b, 2);
    SameCoverage(third.content_damage, b);
    SameCoverage(third.repair_damage, disjoint);
    CommitWithoutAllocation(rotating, std::move(third));
    assert(rotating.SuccessfulSequence() == 3 && rotating.HistorySize() == 2);
    // A third buffer can need both old deltas despite unchanged current content.
    auto restoration = rotating.Plan({}, 3);
    assert(!restoration.content_damage.full && restoration.content_damage.rects.empty());
    SameCoverage(restoration.repair_damage, disjoint);
    CommitWithoutAllocation(rotating, std::move(restoration));
    // Only content was recorded. Prior repair A+B must not pollute the next age2.
    SameCoverage(rotating.Plan(c, 2).repair_damage, c);
    SameCoverage(rotating.Plan(c, 3).repair_damage, runtime::UnionDamage(b, c, size));
    assert(rotating.Plan(c, 4).repair_damage.full);
    assert(rotating.Plan(c, 0).repair_damage.full);
    assert(rotating.Plan(c, std::nullopt).repair_damage.full);
    SameCoverage(rotating.Plan(c, std::nullopt).content_damage, c);
    const auto unchanged_sequence = rotating.SuccessfulSequence();
    const auto unchanged_epoch = rotating.Epoch();
    for (int i = 0; i < 5; ++i) {
        (void)rotating.Plan({}, 1); // Preparing, State and None do not advance history.
    }
    assert(rotating.SuccessfulSequence() == unchanged_sequence &&
           rotating.Epoch() == unchanged_epoch);

    BufferDamageHistory bounded;
    bounded.Reset(size);
    auto baseline = bounded.Plan(DamageRegion::Full(), std::nullopt);
    CommitWithoutAllocation(bounded, std::move(baseline));
    for (int i = 0; i < 10; ++i) {
        auto plan = bounded.Plan(Delta(i * 8, 20, 2, 2), 1);
        CommitWithoutAllocation(bounded, std::move(plan));
    }
    assert(bounded.SuccessfulSequence() == 11 && bounded.HistorySize() == 8);
    const auto oldest_available = bounded.Plan({}, 9);
    assert(!oldest_available.repair_damage.full &&
           runtime::DamageArea(oldest_available.repair_damage, size) == 32);
    assert(!Covers(oldest_available.repair_damage, 0, 20) &&
           Covers(oldest_available.repair_damage, 16, 20));
    assert(bounded.Plan({}, 10).repair_damage.full);
    auto stale = bounded.Plan(a, 1);
    const auto before_reset = bounded.SuccessfulSequence();
    bounded.Reset({50, 50});
    assert(!bounded.Commit(std::move(stale)) && bounded.SuccessfulSequence() == before_reset);
    auto resized = bounded.Plan({}, 1);
    assert(resized.repair_damage.full && resized.content_damage.rects.empty() &&
           !resized.content_damage.full);
    CommitWithoutAllocation(bounded, std::move(resized));
    const auto before_same_size = bounded.Epoch();
    stale = bounded.Plan(a, 1);
    bounded.Reset(
        {50, 50}); // A fresh WSI identity with identical geometry is still an epoch change.
    assert(bounded.Epoch() != before_same_size && !bounded.Commit(std::move(stale)));
    assert(bounded.Plan(a, 1).repair_damage.full);

    BufferDamageHistory failed;
    failed.Reset(size);
    auto start = failed.Plan(DamageRegion::Full(), 1);
    CommitWithoutAllocation(failed, std::move(start));
    auto good = failed.Plan(a, 1);
    CommitWithoutAllocation(failed, std::move(good));
    auto unsuccessful = failed.Plan(b, 1);
    const auto successful = failed.SuccessfulSequence();
    failed.Invalidate();
    assert(!failed.Commit(std::move(unsuccessful)));
    assert(failed.SuccessfulSequence() == successful && failed.HistorySize() == 0);
    auto retry = failed.Plan(b, 1);
    assert(retry.repair_damage.full);
    SameCoverage(retry.content_damage, b);
    auto duplicate = retry;
    CommitWithoutAllocation(failed, std::move(retry));
    assert(!failed.Commit(std::move(duplicate)) && failed.SuccessfulSequence() == successful + 1);
    SameCoverage(failed.Plan(c, 2).repair_damage, runtime::UnionDamage(b, c, size));
    auto wrong_size = failed.Plan(c, 1);
    wrong_size.size = {80, 80};
    assert(!failed.Commit(std::move(wrong_size)));
    const auto valid_epoch = failed.Epoch();
    bool rejected = false;
    try {
        failed.Reset({0, 100});
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected && failed.Epoch() == valid_epoch);
}
