#pragma once

#include <cmath>
#include <string>

namespace prism::runtime {

struct GestureSpec {
    std::string action;
    double threshold{6}; // Euclidean distance in logical pixels, independent of frame rate.
    bool operator==(const GestureSpec &) const = default;
};

inline bool ValidGestureSpec(const GestureSpec &spec)
{
    return !spec.action.empty() && spec.action.size() <= 128 &&
           spec.action.find('\0') == std::string::npos && std::isfinite(spec.threshold) &&
           spec.threshold >= 0 && spec.threshold <= 1024;
}

} // namespace prism::runtime
