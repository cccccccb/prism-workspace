#include "prism/contracts/display_list.hpp"
#include "prism/contracts/events.hpp"
#include "prism/contracts/launch.hpp"
#include "prism/contracts/app_module.h"
#include <cassert>
#include <type_traits>

using namespace prism::contracts;

int main() {
    static_assert(!std::is_convertible_v<WindowId, ResourceId>);
    static_assert(!std::is_convertible_v<ResourceId, WindowId>);
    static_assert(std::is_same_v<decltype(WindowMetrics{}.buffer_size.width), std::uint32_t>);

    static_assert(!std::is_convertible_v<RequestId, InstanceId>);
    static_assert(std::is_standard_layout_v<PrismHostApiV1>);
    static_assert(std::is_same_v<decltype(LaunchEvent{}.pid), std::uint32_t>);
    assert(!NodeId{});
    assert((NodeId{3, 1}));
    assert((NodeId{3, 1} != NodeId{3, 2}));

    DisplayList frame{WindowId{1}, 1, {FillRect{LogicalRect{0, 0, 50, 20}, Color{10, 20, 30, 255}}}};
    assert(frame.commands.size() == 1);
    assert(std::holds_alternative<FillRect>(frame.commands.front()));

    WindowEvent event = ConfigureEvent{WindowId{1}, WindowMetrics{{50, 20}, {75, 30}, 1.5}};
    assert(std::get<ConfigureEvent>(event).metrics.buffer_size.width == 75);
}
