#include "prism/wm/effect_dependencies.hpp"
#include <algorithm>
#include <cmath>

namespace prism::wm::effects {
bool IsFinite(const Rect &r)
{
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) &&
           std::isfinite(r.height) && std::isfinite(r.x + r.width) && std::isfinite(r.y + r.height);
}

bool IsEmpty(const Rect &r)
{
    return r.width <= 0 || r.height <= 0;
}

bool Intersects(const Rect &a, const Rect &b)
{
    return IsFinite(a) && IsFinite(b) && !IsEmpty(a) && !IsEmpty(b) && a.x < b.x + b.width &&
           b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

Rect Intersection(const Rect &a, const Rect &b)
{
    if (!Intersects(a, b)) {
        return {};
    }
    const double x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::min(a.x + a.width, b.x + b.width) - x,
            std::min(a.y + a.height, b.y + b.height) - y};
}

Rect CaptureFootprint(double x, double y, int width, int height, double guard)
{
    if (width <= 0 || height <= 0 || !std::isfinite(guard) || guard < 0) {
        return {};
    }
    Rect result{x - guard, y - guard, 2.0 * std::ceil(width / 2.0) + 2 * guard,
                2.0 * std::ceil(height / 2.0) + 2 * guard};
    return IsFinite(result) ? result : Rect{};
}

std::optional<Rect> MapRect(const Rect &r, const Rect &source, const Rect &destination)
{
    if (!IsFinite(r) || !IsFinite(source) || !IsFinite(destination) || IsEmpty(source) ||
        IsEmpty(destination)) {
        return std::nullopt;
    }
    const auto clipped = Intersection(r, source);
    if (IsEmpty(clipped)) {
        return Rect{};
    }
    const double sx = destination.width / source.width, sy = destination.height / source.height;
    const Rect result{destination.x + (clipped.x - source.x) * sx,
                      destination.y + (clipped.y - source.y) * sy, clipped.width * sx,
                      clipped.height * sy};
    return IsFinite(result) ? std::optional<Rect>{result} : std::nullopt;
}

bool DamageHistory::Record(std::span<const Rect> damage, std::uint64_t epoch, bool full)
{
    Entry candidate;
    candidate.mapping_epoch = epoch;
    candidate.full = full;
    bool merged = false;
    auto unite = [](const Rect &a, const Rect &b) {
        const double x = std::min(a.x, b.x), y = std::min(a.y, b.y);
        return Rect{x, y, std::max(a.x + a.width, b.x + b.width) - x,
                    std::max(a.y + a.height, b.y + b.height) - y};
    };
    if (!full) {
        for (const auto &rect : damage) {
            if (!IsFinite(rect)) {
                candidate.full = true;
                candidate.count = 0;
                break;
            }
            if (IsEmpty(rect)) {
                continue;
            }
            if (!merged && candidate.count < kMaxRectangles) {
                candidate.rectangles[candidate.count++] = rect;
                continue;
            }
            if (!merged) {
                for (std::size_t i = 1; i < candidate.count; ++i) {
                    candidate.rectangles[0] =
                        unite(candidate.rectangles[0], candidate.rectangles[i]);
                }
                candidate.count = 1;
                merged = true;
            }
            candidate.rectangles[0] = unite(candidate.rectangles[0], rect);
            if (!IsFinite(candidate.rectangles[0])) {
                candidate.full = true;
                candidate.count = 0;
                break;
            }
        }
    }
    if (!candidate.full && !candidate.count) {
        return false;
    }
    candidate.revision = ++revision_;
    entries_[next_] = candidate;
    next_ = (next_ + 1) % kHistoryLength;
    count_ = std::min(count_ + 1, kHistoryLength);
    return true;
}

DamageHistory::QueryResult DamageHistory::Since(std::uint64_t observed, const Rect &query,
                                                std::uint64_t epoch) const
{
    if (observed > revision_ || !IsFinite(query)) {
        return {true, true};
    }
    if (observed == revision_ || IsEmpty(query)) {
        return {};
    }
    const auto oldest = (next_ + kHistoryLength - count_) % kHistoryLength;
    if (!count_ || observed + 1 < entries_[oldest].revision) {
        return {true, true};
    }
    for (std::size_t i = 0; i < count_; ++i) {
        const auto &entry = entries_[(oldest + i) % kHistoryLength];
        if (entry.revision <= observed) {
            continue;
        }
        if (entry.mapping_epoch != epoch) {
            return {true, true};
        }
        if (entry.full) {
            return {true, false};
        }
        for (std::size_t j = 0; j < entry.count; ++j) {
            if (Intersects(entry.rectangles[j], query)) {
                return {true, false};
            }
        }
    }
    return {};
}

DependencyObservation Observe(const DamageHistory &history, DependencyStamp previous,
                              std::uint64_t identity, const Rect &query, std::uint64_t epoch)
{
    DependencyObservation result;
    result.stamp = {identity, history.Revision(), previous.sampled_revision, epoch};
    if (previous.identity != identity) {
        result.changed = true;
        result.stamp.sampled_revision = history.Revision();
        return result;
    }
    if (previous.mapping_epoch != epoch) {
        result.changed = true;
        result.fallback = true;
        result.stamp.sampled_revision = history.Revision();
        return result;
    }
    const auto damage = history.Since(previous.observed_revision, query, epoch);
    result.changed = damage.changed;
    result.fallback = damage.fallback;
    if (damage.changed) {
        result.stamp.sampled_revision = history.Revision();
    } else {
        result.advanced_without_damage = previous.observed_revision != history.Revision();
    }
    return result;
}
} // namespace prism::wm::effects
