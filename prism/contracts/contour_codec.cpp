#include "prism/contracts/contour.hpp"
#include <bit>
#include <stdexcept>

namespace prism::contracts {
namespace {
void Write(std::vector<std::uint8_t> &payload, std::uint32_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i) {
        payload.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    }
}

std::uint32_t Read(std::span<const std::uint8_t> payload, std::size_t at, unsigned bytes)
{
    std::uint32_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) {
        value |= static_cast<std::uint32_t>(payload[at + i]) << (i * 8);
    }
    return value;
}
} // namespace

std::vector<std::uint8_t> EncodeContour(const Contour &contour)
{
    ValidateContour(contour);

    std::vector<std::uint8_t> payload;
    payload.reserve(8 + contour.points.size() * 8);
    Write(payload, 1, 2);
    Write(payload, static_cast<std::uint32_t>(contour.points.size()), 2);
    Write(payload, 0, 4);
    for (auto point : contour.points) {
        Write(payload, std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(point.x * 256)), 4);
        Write(payload, std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(point.y * 256)), 4);
    }
    return payload;
}

Contour DecodeContour(std::span<const std::uint8_t> payload)
{
    if (payload.size() < 8 || payload.size() > 8 + ContourVertexLimit * 8 ||
        Read(payload, 0, 2) != 1 || Read(payload, 4, 4) != 0) {
        throw std::invalid_argument("Contour payload has invalid size/version/flags");
    }
    const auto count = Read(payload, 2, 2);
    if (count < 3 || count > ContourVertexLimit || payload.size() != 8 + count * 8) {
        throw std::invalid_argument("Contour payload vertex count/length mismatch");
    }

    Contour contour;
    contour.points.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto x = std::bit_cast<std::int32_t>(Read(payload, 8 + i * 8, 4));
        const auto y = std::bit_cast<std::int32_t>(Read(payload, 12 + i * 8, 4));
        contour.points.push_back({static_cast<double>(x) / 256, static_cast<double>(y) / 256});
    }

    ValidateContour(contour);
    return contour;
}
} // namespace prism::contracts
