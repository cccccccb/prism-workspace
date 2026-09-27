#pragma once
#include "prism/contracts/damage.hpp"
#include "prism/contracts/types.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace prism::runtime {
struct DamageLimits {
    std::size_t max_rects{16};
    double max_area_ratio{0.75};
};
// Buffer-pixel coordinates. Clip and remove overlaps; excessive fragmentation
// or covered area conservatively becomes Full. Full always has no rectangles.
contracts::DamageRegion NormalizeDamage(const contracts::DamageRegion&,
    contracts::BufferSize, DamageLimits = {});
contracts::DamageRegion UnionDamage(std::span<const contracts::DamageRegion>,
    contracts::BufferSize, DamageLimits = {});
contracts::DamageRegion UnionDamage(const contracts::DamageRegion&,
    const contracts::DamageRegion&, contracts::BufferSize, DamageLimits = {});
// Exact clipped union area, without applying the fragmentation/area fallback.
std::uint64_t DamageArea(const contracts::DamageRegion&, contracts::BufferSize);
// Convert logical ink bounds outwards into buffer pixels before clipping.
// Invalid/non-finite geometry conservatively returns Full; invalid scale throws.
contracts::DamageRegion DamageFromLogicalBounds(std::span<const contracts::LogicalRect>,
    contracts::BufferSize, double scale = 1.0, DamageLimits = {});

struct BufferDamagePlan {
    contracts::BufferSize size{};
    std::uint64_t epoch{}, next_sequence{};
    contracts::DamageRegion content_damage;
    contracts::DamageRegion repair_damage;
};
// Histories contain only successful pixel content changes, never repair regions.
// None/State do not call Commit. First-frame content Full comes from the producer;
// losing buffer history upgrades repair only, including restoration-only Pixels.
class BufferDamageHistory {
public:
    explicit BufferDamageHistory(std::size_t capacity = 8, DamageLimits = {});
    BufferDamageHistory(const BufferDamageHistory&) = delete;
    BufferDamageHistory& operator=(const BufferDamageHistory&) = delete;
    // Reset even with the same size for a new WSI target. Epoch invalidates old
    // plans; the lifetime successful-pixel sequence does not reset.
    void Reset(contracts::BufferSize);
    void Invalidate() noexcept; // Failed Swap: no successful content advancement.
    BufferDamagePlan Plan(const contracts::DamageRegion& content_damage,
        std::optional<unsigned> buffer_age) const;
    // Call only after successful Swap. All ring slots were allocated before any
    // Plan; moving its normalized content performs no allocation after Swap.
    bool Commit(BufferDamagePlan&&) noexcept;
    std::uint64_t Epoch() const noexcept { return epoch_; }
    std::uint64_t SuccessfulSequence() const noexcept { return successful_sequence_; }
    std::size_t HistorySize() const noexcept { return history_size_; }
    contracts::BufferSize Size() const noexcept { return size_; }
private:
    std::vector<contracts::DamageRegion> history_;
    DamageLimits limits_;
    contracts::BufferSize size_{};
    std::uint64_t epoch_{1}, successful_sequence_{};
    std::size_t next_index_{}, history_size_{};
    bool history_valid_{};
};
} // namespace prism::runtime
