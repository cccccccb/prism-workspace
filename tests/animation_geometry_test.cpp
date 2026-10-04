#include "prism/animation/geometry.hpp"
#include <cassert>
#include <cmath>
#include <limits>
using namespace prism;

namespace {
class Clock final : public animation::AnimationClock {
public:
    animation::MonotonicTimeNs NowNs() const noexcept override
    {
        return 0;
    }
};

bool Near(double a, double b)
{
    return std::abs(a - b) < 0.00001;
}
} // namespace

int main()
{
    Clock clock;
    animation::GeometryTimeline timeline(clock);
    const contracts::LogicalRect original{20, 40, 100, 200}, full{0, 0, 800, 600};
    assert(!timeline.Retarget(full, {100}, 1000));
    assert(!timeline.Reset({0, 0, 0, 10}));
    assert(timeline.Reset(original));
    assert(timeline.Sample(1000).bounds == original);
    assert(timeline.Retarget(full, {100}, 1000));
    const auto quarter = timeline.Sample(1025);
    assert(Near(quarter.bounds.x, 15) && Near(quarter.bounds.width, 275));
    const auto generation = quarter.generation;
    assert(timeline.Retarget(full, {100}, 1030));
    assert(timeline.Sample(1030).generation == generation);

    assert(timeline.Retarget(original, {100}, 1050));
    const auto reverse = timeline.Sample(1050);
    assert(Near(reverse.bounds.x, 10) && Near(reverse.bounds.width, 450));
    assert(reverse.generation > generation);
    assert(Near(timeline.Sample(1075).bounds.width, 362.5));
    // Input follows the retained submitted sample even when the clock advances.
    const auto mapped =
        animation::GeometryTimeline::ToSurface(quarter, {0, 0, 100, 200},
                                               {quarter.bounds.x + quarter.bounds.width / 2,
                                                quarter.bounds.y + quarter.bounds.height / 2});
    assert(Near(mapped.x, 50) && Near(mapped.y, 100));
    assert(!timeline.Retarget({0, 0, std::numeric_limits<double>::infinity(), 10}, {100}, 1075));
    assert(timeline.Sample(1150).bounds == original && !timeline.Running());
    assert(timeline.Retarget(full, {0}, 1200));
    assert(timeline.Sample(1200).bounds == full && !timeline.Running());
    const auto before_reset = timeline.Sample(1200).generation;
    assert(timeline.Reset(original));
    assert(timeline.Sample(1200).generation > before_reset && !timeline.Running());
    // Sparse samples arrive at exactly the same endpoint as dense sampling.
    animation::GeometryTimeline sparse(clock);
    assert(sparse.Reset(original));
    assert(sparse.Retarget(full, {100}, 1000));
    assert(sparse.Sample(10000).bounds == full && !sparse.Running());
}
