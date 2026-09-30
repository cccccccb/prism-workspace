#include "prism/contracts/layout_snapshot.hpp"

namespace prism::contracts {
namespace {
struct CodePoint {
    std::uint32_t value{};
    std::size_t size{1};
    bool valid{};
};

CodePoint ReadCodePoint(std::string_view text)
{
    const auto first = static_cast<unsigned char>(text.front());
    if (first < 0x80) {
        return {first, 1, true};
    }

    unsigned continuation{};
    std::uint32_t value{}, minimum{};
    if ((first & 0xe0) == 0xc0) {
        continuation = 1;
        value = first & 31;
        minimum = 128;
    } else if ((first & 0xf0) == 0xe0) {
        continuation = 2;
        value = first & 15;
        minimum = 2048;
    } else if ((first & 0xf8) == 0xf0) {
        continuation = 3;
        value = first & 7;
        minimum = 65536;
    } else {
        return {};
    }
    if (continuation >= text.size()) {
        return {};
    }
    for (unsigned i = 1; i <= continuation; ++i) {
        const auto next = static_cast<unsigned char>(text[i]);
        if ((next & 0xc0) != 0x80) {
            return {};
        }
        value = (value << 6) | (next & 63);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
        return {};
    }
    return {value, continuation + 1, true};
}
} // namespace

std::string LayoutDisplayName(std::string_view text)
{
    constexpr std::size_t limit = 128;
    constexpr std::string_view replacement = "\xef\xbf\xbd";
    std::string result;
    result.reserve(limit);

    while (!text.empty() && result.size() < limit) {
        const auto point = ReadCodePoint(text);
        const bool control = point.value < 0x20 || (point.value >= 0x7f && point.value <= 0x9f);
        const auto encoded = point.valid && !control ? text.substr(0, point.size) : replacement;
        if (encoded.size() > limit - result.size()) {
            break;
        }
        result.append(encoded);
        text.remove_prefix(point.size);
    }
    return result.empty() ? "Unnamed" : result;
}
} // namespace prism::contracts
