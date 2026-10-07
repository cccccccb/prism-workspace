#pragma once

#include "prism/contracts/types.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace prism::contracts {

struct SurfaceInputRegion;

inline constexpr std::size_t ContourVertexLimit = 256;
inline constexpr double ContourCoordinateLimit = 8192;
inline constexpr double ContourFlattenTolerance = 0.25;

// One simple, implicitly closed polygon, quantized to 1/256 logical pixel.
// Either winding is accepted; no holes, repeated vertices or self-touching edges.
// Validate at installation/transport boundaries before using geometry queries.
struct Contour {
    std::vector<LogicalPoint> points;
    bool operator==(const Contour &) const noexcept = default;
};

struct ContourLine {
    LogicalPoint end{};
};

struct ContourCubic {
    LogicalPoint control1{};
    LogicalPoint control2{};
    LogicalPoint end{};
};

// Preparation input only. Curves are flattened once by the geometry owner;
// rendering and transport consume the resulting polygon without re-flattening.
struct ContourPath {
    LogicalPoint start{};
    std::vector<std::variant<ContourLine, ContourCubic>> segments;
};

// All rejected input throws std::invalid_argument. Preparation has bounded
// subdivision (depth 10), <=0.25 pre-quantization deviation and the vertex budget.
void ValidateContour(const Contour &contour);
Contour PrepareContour(const ContourPath &path);

// Queries require a validated polygon. Boundary inclusion is explicit, including
// at concave vertices. Non-finite query points are always outside.
LogicalRect ContourBounds(const Contour &contour);
bool ContourContains(const Contour &contour, LogicalPoint point, bool include_edges = true);

// Pixel-center binary mask in logical pixels, intersected with the clip and each
// contour. Equal spans on adjacent rows coalesce. Validates <=8 contours and
// rejects masks exceeding 65536 rectangles; empty contour span means no mask.
std::vector<LogicalRect> RasterizeContourIntersection(std::span<const Contour> contours,
                                                      LogicalRect clip);
// Exact pixel-center intersection with analytic rounded clips. Combined shape
// count is at most eight; the rectangular clip remains half-open.
std::vector<LogicalRect> RasterizeContourIntersection(std::span<const Contour> contours,
                                                      LogicalRect clip,
                                                      std::span<const SurfaceInputRegion> rounded);

// Backend-independent payload v1: 8-byte little-endian header (u16 version,
// count, flags=0, reserved=0), followed by signed i32 x/y in Q24.8. <=2056 bytes.
// Both boundaries validate geometry; trailing bytes and unknown versions fail.
std::vector<std::uint8_t> EncodeContour(const Contour &contour);
Contour DecodeContour(std::span<const std::uint8_t> payload);

} // namespace prism::contracts
