#pragma once

#include "prism/runtime/blueprint.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace prism::runtime {
inline constexpr std::size_t kMaxTooltipTextBytes = 256;

inline bool IsTooltipContentKind(Kind kind) noexcept
{
    return kind == Kind::Row || kind == Kind::Column || kind == Kind::Box || kind == Kind::Visual ||
           kind == Kind::Text || kind == Kind::Icon || kind == Kind::Image ||
           kind == Kind::Separator;
}

inline bool ValidTooltipText(std::string_view text) noexcept
{
    if (text.size() > kMaxTooltipTextBytes) {
        return false;
    }

    for (std::size_t at = 0; at < text.size();) {
        const auto first = static_cast<unsigned char>(text[at++]);
        std::uint32_t point = first;
        std::uint32_t minimum{};
        unsigned remaining{};
        if (first >= 0x80) {
            if ((first & 0xe0) == 0xc0) {
                point = first & 0x1f;
                remaining = 1;
                minimum = 0x80;
            } else if ((first & 0xf0) == 0xe0) {
                point = first & 0x0f;
                remaining = 2;
                minimum = 0x800;
            } else if ((first & 0xf8) == 0xf0) {
                point = first & 0x07;
                remaining = 3;
                minimum = 0x10000;
            } else {
                return false;
            }
            if (remaining > text.size() - at) {
                return false;
            }
            while (remaining--) {
                const auto byte = static_cast<unsigned char>(text[at++]);
                if ((byte & 0xc0) != 0x80) {
                    return false;
                }
                point = (point << 6) | (byte & 0x3f);
            }
        }
        if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff) ||
            (point != '\n' && point < 0x20) || (point >= 0x7f && point <= 0x9f) ||
            point == 0x2028 || point == 0x2029) {
            return false;
        }
    }
    return true;
}
} // namespace prism::runtime
