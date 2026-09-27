#pragma once

#include "prism/contracts/types.hpp"
#include <cstdint>
#include <string>
#include <variant>

namespace prism::contracts {

enum class ButtonState : std::uint8_t { Released, Pressed };
enum class PointerButton : std::uint8_t { Primary, Secondary, Middle, Back, Forward, Other };

struct PointerMotionEvent {
    WindowId window{};
    LogicalPoint position{};
    std::uint64_t time_ns{0};
};

struct PointerButtonEvent {
    WindowId window{};
    LogicalPoint position{};
    PointerButton button{PointerButton::Primary};
    ButtonState state{ButtonState::Released};
    std::uint32_t other_button_code{0};
    std::uint64_t time_ns{0};
};

struct PointerScrollEvent {
    WindowId window{};
    LogicalPoint position{};
    double delta_x{0.0};
    double delta_y{0.0};
    std::uint64_t time_ns{0};
};

struct KeyEvent {
    WindowId window{};
    std::uint32_t physical_key{0}; // USB HID usage, normalized by the platform adapter.
    ButtonState state{ButtonState::Released};
    bool repeat{false};
    std::uint64_t time_ns{0};
};

struct TextInputEvent {
    WindowId window{};
    std::string utf8;
    std::uint64_t time_ns{0};
};

struct ConfigureEvent {
    WindowId window{};
    WindowMetrics metrics{};
};

struct FocusEvent {
    WindowId window{};
    bool focused{false};
};

struct CloseRequestedEvent {
    WindowId window{};
};

using WindowEvent =
    std::variant<PointerMotionEvent, PointerButtonEvent, PointerScrollEvent, KeyEvent,
                 TextInputEvent, ConfigureEvent, FocusEvent, CloseRequestedEvent>;

} // namespace prism::contracts
