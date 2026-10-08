#include "prism/runtime/task_paint.hpp"

#include <cmath>
#include <stdexcept>

namespace prism::runtime {

bool MatchesTaskPaintEnvironment(const TaskPaintSource &source,
                                 const TaskPaintSource &current) noexcept
{
    return source.identity.task.owner && source.identity.task.request && source.identity.ui.owner &&
           source.identity.ui.generation && source.identity.cycle &&
           source.identity == current.identity && source.configure_count > 0 &&
           source.configure_count == current.configure_count && source.buffer_size.width > 0 &&
           source.buffer_size.height > 0 && source.buffer_size.width == current.buffer_size.width &&
           source.buffer_size.height == current.buffer_size.height && std::isfinite(source.scale) &&
           source.scale > 0 && source.scale == current.scale &&
           source.theme_generation == current.theme_generation &&
           source.resource_epoch == current.resource_epoch;
}

contracts::DisplayList ComposeTaskPaint(const contracts::DisplayList &body,
                                        const TaskPaintFragment &paint, double opacity)
{
    if (!std::isfinite(opacity) || opacity < 0 || opacity > 1) {
        throw std::invalid_argument("Task paint opacity must be finite and in [0,1]");
    }

    auto result = body;
    if (opacity == 0 || paint.commands.empty()) {
        return result;
    }
    if (opacity < 1) {
        result.commands.emplace_back(contracts::PushOpacity{opacity});
    }
    result.commands.insert(result.commands.end(), paint.commands.begin(), paint.commands.end());
    if (opacity < 1) {
        result.commands.emplace_back(contracts::PopOpacity{});
    }
    return result;
}

} // namespace prism::runtime
