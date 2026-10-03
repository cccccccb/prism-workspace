#pragma once

#include <cstdint>

namespace prism::contracts {

// These identifiers are local to one client runtime. They are not wire handles.
template <typename Tag> struct LocalId {
    std::uint64_t value{0};

    constexpr explicit operator bool() const noexcept
    {
        return value != 0;
    }

    constexpr bool operator==(const LocalId &) const noexcept = default;
};

struct WindowTag;
struct ResourceTag;
using WindowId = LocalId<WindowTag>;
using ResourceId = LocalId<ResourceTag>;

// An index can be reused only after its generation changes. Generation zero
// and the all-ones index are reserved for an invalid node.
struct NodeId {
    std::uint32_t index{UINT32_MAX};
    std::uint32_t generation{0};

    constexpr explicit operator bool() const noexcept
    {
        return index != UINT32_MAX && generation != 0;
    }

    constexpr bool operator==(const NodeId &) const noexcept = default;
};

struct LogicalPoint {
    double x{0.0};
    double y{0.0};
    constexpr bool operator==(const LogicalPoint &) const noexcept = default;
};

struct LogicalSize {
    double width{0.0};
    double height{0.0};
    constexpr bool operator==(const LogicalSize &) const noexcept = default;
};

struct LogicalRect {
    double x{0.0};
    double y{0.0};
    double width{0.0};
    double height{0.0};
    constexpr bool operator==(const LogicalRect &) const noexcept = default;
};

struct BufferSize {
    std::uint32_t width{0};
    std::uint32_t height{0};
};

struct WindowMetrics {
    LogicalSize logical_size{};
    BufferSize buffer_size{};
    double scale{1.0};
};

enum class WindowRole : std::uint8_t {
    Toplevel,
    Desktop,
    TopBar,
    Dock,
    LayoutControls,
};

} // namespace prism::contracts
