#pragma once

#include <algorithm>
#include <cmath>

namespace prism::tree {

// Shared by actual placement and boundary validation, including tiny-area fallback.
struct SplitLayoutMetrics {
    float axis{0};
    float minimum{0};
    float gap{0};
    float usable{0};
    int count{0};
};

inline SplitLayoutMetrics SplitMetrics(float extent, int count, int inner_gap)
{
    SplitLayoutMetrics metrics;
    metrics.axis = std::max(0.0f, extent);
    metrics.count = count;
    if (count <= 0) {
        return metrics;
    }

    metrics.minimum = std::min(1.0f, metrics.axis / count);
    metrics.gap =
        count > 1
            ? std::min(float(std::max(0, inner_gap)),
                       std::max(0.0f,
                                std::floor((metrics.axis - count * metrics.minimum) / (count - 1))))
            : 0;
    metrics.usable = std::max(0.0f, metrics.axis - (count - 1) * metrics.gap);
    return metrics;
}

inline float SplitExtent(const SplitLayoutMetrics &metrics, float remaining, int index,
                         double fraction)
{
    const float desired = metrics.axis >= metrics.count
                              ? std::round(metrics.usable * float(fraction))
                              : metrics.usable * float(fraction);
    const float maximum = std::max(
        metrics.minimum, remaining - (metrics.count - index - 1) * (metrics.minimum + metrics.gap));
    return index == metrics.count - 1 ? remaining : std::clamp(desired, metrics.minimum, maximum);
}

} // namespace prism::tree
