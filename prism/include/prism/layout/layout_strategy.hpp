#pragma once

#include "prism/core/types.hpp"
#include <string>

namespace prism::layout {

/**
 * @brief Strategy Pattern: Interface for swappable window layout algorithms
 */
class LayoutStrategy {
public:
    virtual ~LayoutStrategy() = default;

    virtual std::string GetStrategyName() const = 0;
    virtual void CalculateLayout(core::Rect screen_bounds, core::Rect &win_a,
                                 core::Rect &win_b) = 0;

    virtual void CalculateMultiLayout(core::Rect screen_bounds, size_t count,
                                      std::vector<core::Rect> &out_bounds)
    {
        out_bounds.clear();
        if (count == 0) {
            return;
        }
        if (count == 1) {
            out_bounds.push_back(screen_bounds);
            return;
        }
        core::Rect a, b;
        CalculateLayout(screen_bounds, a, b);
        out_bounds.push_back(a);
        out_bounds.push_back(b);
        for (size_t i = 2; i < count; ++i) {
            out_bounds.push_back(core::Rect{0, 0, 0, 0});
        }
    }

    virtual void StepPhysics(float dt) = 0;
};

} // namespace prism::layout
