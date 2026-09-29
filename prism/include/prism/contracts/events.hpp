#pragma once

#include "prism/contracts/types.hpp"
#include <cstdint>
#include <string>
#include <variant>

namespace prism::contracts {

enum class ButtonState : std::uint8_t { Released, Pressed };
enum class PointerButton : std::uint8_t { Primary, Secondary, Middle, Back, Forward, Other };

// Adapter-local identities. A recreated logical device receives a new
// generation even when the platform reuses its protocol object name.
struct InputSource {
    std::uint64_t seat{};
    std::uint64_t device{};
    std::uint64_t generation{};

    bool operator==(const InputSource &) const = default;
};

struct KeyModifiers {
    bool shift{};
    bool control{};
    bool alt{};
    bool meta{};
};

// Event times are the adapter's local monotonic receipt time. They do not
// claim to be converted compositor protocol timestamps or presentation time.
struct PointerEnterEvent {
    WindowId window{};
    LogicalPoint position{};
    std::uint64_t time_ns{0};
    InputSource source{};
};

struct PointerLeaveEvent {
    WindowId window{};
    std::uint64_t time_ns{0};
    InputSource source{};
};

struct PointerCancelEvent {
    WindowId window{};
    std::uint64_t time_ns{0};
    InputSource source{};
};

struct PointerMotionEvent {
    WindowId window{};
    LogicalPoint position{};
    std::uint64_t time_ns{0};
    InputSource source{};
};

struct PointerButtonEvent {
    WindowId window{};
    LogicalPoint position{};
    PointerButton button{PointerButton::Primary};
    ButtonState state{ButtonState::Released};
    std::uint32_t other_button_code{0};
    std::uint64_t time_ns{0};
    InputSource source{};
};

struct PointerScrollEvent {
    WindowId window{};
    LogicalPoint position{};
    double delta_x{0.0};
    double delta_y{0.0};
    std::uint64_t time_ns{0};
    InputSource source{};
};

struct KeyEvent {
    WindowId window{};
    std::uint32_t physical_key{0}; // USB HID usage, normalized by the platform adapter.
    ButtonState state{ButtonState::Released};
    bool repeat{false};
    std::uint64_t time_ns{0};
    InputSource source{};
    KeyModifiers modifiers{};
};

struct TextInputEvent {
    WindowId window{};
    std::string utf8;
    std::uint64_t time_ns{0};
};

struct ConfigureEvent {
    WindowId window{};
    WindowMetrics metrics{};
    int configure_count{};
};

struct FocusEvent {
    WindowId window{};
    bool focused{false};
    InputSource source{};
};

struct CloseRequestedEvent {
    WindowId window{};
};

using WindowEvent =
    std::variant<PointerEnterEvent, PointerLeaveEvent, PointerCancelEvent, PointerMotionEvent,
                 PointerButtonEvent, PointerScrollEvent, KeyEvent, TextInputEvent, ConfigureEvent,
                 FocusEvent, CloseRequestedEvent>;

} // namespace prism::contracts
