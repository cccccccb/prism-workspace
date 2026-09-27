#include "prism/wm/effect_dependencies.hpp"
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

using namespace prism::wm::effects;

namespace {
bool Near(double actual, double expected)
{
    return std::abs(actual - expected) < 1e-9;
}

void ExpectRect(const Rect &actual, const Rect &expected)
{
    assert(Near(actual.x, expected.x) && Near(actual.y, expected.y));
    assert(Near(actual.width, expected.width) && Near(actual.height, expected.height));
}

void Record(DamageHistory &history, Rect damage, std::uint64_t epoch = 7)
{
    const std::array rectangles{damage};
    assert(history.Record(rectangles, epoch));
}

void OutsideDamageAdvancesObservationWithoutChangingSamples()
{
    DamageHistory history;
    const Rect sampled{0, 0, 40, 40};
    Record(history, {10, 10, 2, 2});
    auto initial = Observe(history, {}, 42, sampled, 7);
    assert(initial.changed && initial.stamp.identity == 42);
    assert(initial.stamp.observed_revision == history.Revision());
    const auto published = initial.stamp;

    Record(history, {100, 100, 4, 4});
    auto outside = Observe(history, published, 42, sampled, 7);
    assert(!outside.changed && !outside.fallback && outside.advanced_without_damage);
    assert(outside.stamp.observed_revision == history.Revision());
    assert(outside.stamp.sampled_revision == published.sampled_revision);
    // Querying is pure: the caller owns publication of the candidate stamp.
    assert(published == initial.stamp);

    Record(history, {20, 20, 1, 1});
    auto inside = Observe(history, outside.stamp, 42, sampled, 7);
    assert(inside.changed && !inside.fallback && !inside.advanced_without_damage);
    assert(inside.stamp.sampled_revision > outside.stamp.sampled_revision);
    assert(inside.stamp.observed_revision == history.Revision());
    auto unchanged = Observe(history, inside.stamp, 42, sampled, 7);
    assert(!unchanged.changed && !unchanged.advanced_without_damage);
    assert(unchanged.stamp == inside.stamp);
}

void BufferReplacementDoesNotInventPixelDamage()
{
    DamageHistory history;
    const Rect sampled{0, 0, 20, 20};
    Record(history, sampled);
    const auto cached = Observe(history, {}, 17, sampled, 7).stamp;
    const auto revision = history.Revision();
    // The stable surface identity and mapping epoch are unchanged. A new
    // same-size backing buffer with no declared damage supplies no pixel event.
    assert(!history.Record({}, 7));
    assert(history.Revision() == revision);
    const auto replacement = Observe(history, cached, 17, sampled, 7);
    assert(!replacement.changed && !replacement.fallback);
    assert(replacement.stamp == cached);
    // A different surface may occupy the same geometry; it cannot reuse this
    // dependency even if its revision happens to have the same numeric value.
    assert(Observe(history, cached, 18, sampled, 7).changed);
}

void IndependentConsumersAndUnpublishedCandidates()
{
    DamageHistory history;
    const Rect left{0, 0, 40, 40}, right{100, 0, 40, 40};
    assert(history.Record({}, 7, true));
    auto left_cached = Observe(history, {}, 91, left, 7).stamp;
    auto right_cached = Observe(history, {}, 91, right, 7).stamp;
    Record(history, {10, 10, 2, 2});
    const auto left_candidate = Observe(history, left_cached, 91, left, 7);
    const auto right_candidate = Observe(history, right_cached, 91, right, 7);
    assert(left_candidate.changed && !right_candidate.changed);
    assert(right_candidate.advanced_without_damage);
    right_cached = right_candidate.stamp;

    // Discard the left candidate, as a caller must after a failed region
    // render. Neither observing another consumer nor discarding a candidate
    // consumes the history needed by that older published cache.
    const auto retry = Observe(history, left_cached, 91, left, 7);
    assert(retry.changed && retry.stamp == left_candidate.stamp);
    assert(left_cached.observed_revision < right_cached.observed_revision);
    Record(history, {110, 10, 2, 2});
    assert(Observe(history, right_cached, 91, right, 7).changed);
    assert(Observe(history, left_cached, 91, left, 7).changed);
    left_cached = Observe(history, left_cached, 91, left, 7).stamp;
    assert(!Observe(history, left_cached, 91, left, 7).changed);
}

void LostHistoryAndMappingChangesAreConservative()
{
    DamageHistory history;
    const Rect sampled{0, 0, 20, 20};
    Record(history, sampled);
    const auto lagging = Observe(history, {}, 31, sampled, 7).stamp;
    Record(history, {100, 0, 1, 1});
    const auto retained_boundary = Observe(history, lagging, 31, sampled, 7).stamp;
    for (std::size_t i = 0; i < DamageHistory::kHistoryLength; ++i) {
        Record(history, {100 + static_cast<double>(i), 0, 1, 1});
    }
    const auto lost = Observe(history, lagging, 31, sampled, 7);
    assert(lost.changed && lost.fallback);
    // The entire interval after this newer watermark remains available.
    const auto retained = Observe(history, retained_boundary, 31, sampled, 7);
    assert(!retained.changed && !retained.fallback && retained.advanced_without_damage);
    assert(retained.stamp.sampled_revision == retained_boundary.sampled_revision);

    // A changed mapping cannot rely on empty or nonintersecting pixel damage,
    // even if the consumer already observed the latest content revision.
    const auto remapped = Observe(history, retained.stamp, 31, sampled, 8);
    assert(remapped.changed && remapped.fallback);
    assert(!Observe(history, remapped.stamp, 31, sampled, 8).changed);
    assert(history.Record({}, 8, true));
    const auto full = Observe(history, retained.stamp, 31, sampled, 8);
    assert(full.changed);
}

void DamageIsCopiedAndLargeRegionsCannotDropChanges()
{
    DamageHistory history;
    std::array damage{Rect{100, 0, 1, 1}};
    assert(history.Record(damage, 3));
    damage.front() = {0, 0, 1, 1};
    assert(!history.Since(0, {0, 0, 10, 10}, 3).changed);

    std::vector<Rect> many;
    for (std::size_t i = 0; i < DamageHistory::kMaxRectangles + 1; ++i) {
        many.push_back({static_cast<double>(i) * 10, 50, 1, 1});
    }
    const auto before = history.Revision();
    assert(history.Record(many, 3));
    for (const auto &changed : many) {
        assert(history.Since(before, changed, 3).changed);
    }
    const std::array invalid{Rect{0, 0, std::numeric_limits<double>::quiet_NaN(), 1}};
    const auto before_invalid = history.Revision();
    assert(history.Record(invalid, 3));
    assert(history.Since(before_invalid, {1000, 1000, 1, 1}, 3).changed);
}

void FractionalProjectionAndSamplingBoundaries()
{
    const Rect source{10, 20, 40, 20}, destination{100, 200, 80, 60};
    const auto projected = MapRect({15, 30, 20, 10}, source, destination);
    assert(projected);
    ExpectRect(*projected, {110, 230, 40, 30});
    const auto clipped = MapRect({0, 10, 20, 20}, source, destination);
    assert(clipped);
    ExpectRect(*clipped, {100, 200, 20, 30});
    const auto fractional =
        MapRect({0.5, 1, 0.25, 0.5}, {0.25, 0.75, 2.5, 3.25}, {5.125, -2.25, 10, 13});
    assert(fractional);
    ExpectRect(*fractional, {6.125, -1.25, 1, 2});
    const auto touches = MapRect({50, 20, 4, 5}, source, destination);
    assert(touches && IsEmpty(*touches));
    assert(!MapRect({0, 0, 1, 1}, {0, 0, 0, 1}, destination));
    assert(!MapRect({0, 0, 1, 1}, source, {0, 0, std::numeric_limits<double>::infinity(), 1}));

    const Rect sample{0, 0, 10, 10};
    assert(!Intersects(sample, {10, 0, 1, 1}));
    assert(!Intersects(sample, {0, 10, 1, 1}));
    assert(Intersects(sample, {9.999, 0, 0.01, 1}));
    const auto guarded = CaptureFootprint(10, 20, 6, 4);
    assert(Intersects(guarded, {8.5, 20, 0.1, 0.1}));
    assert(!Intersects(guarded, {7.5, 20, 0.1, 0.1}));
    DamageHistory guard_history;
    Record(guard_history, {8.5, 20, 0.1, 0.1});
    assert(!guard_history.Since(0, {10, 20, 6, 4}, 7).changed);
    assert(guard_history.Since(0, guarded, 7).changed);
    // An odd result dimension allocates ceil(width / 2) capture texels.
    // The last half-resolution texel still samples the adjacent logical pixel.
    const auto odd = CaptureFootprint(10, 20, 5, 3, 0);
    assert(Intersects(odd, {15.5, 20, 0.1, 0.1}));
    assert(!Intersects(odd, {16, 20, 0.1, 0.1}));

    DamageHistory history;
    Record(history, {10, 0, 1, 1});
    assert(!history.Since(0, sample, 7).changed);
    Record(history, {9.999, 0, 0.01, 1});
    assert(history.Since(1, sample, 7).changed);
}
} // namespace

int main()
{
    OutsideDamageAdvancesObservationWithoutChangingSamples();
    BufferReplacementDoesNotInventPixelDamage();
    IndependentConsumersAndUnpublishedCandidates();
    LostHistoryAndMappingChangesAreConservative();
    DamageIsCopiedAndLargeRegionsCannotDropChanges();
    FractionalProjectionAndSamplingBoundaries();
    std::cout
        << "Effect dependency history, independent publication and sampling boundaries passed\n";
}
