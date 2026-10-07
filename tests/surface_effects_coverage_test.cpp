#include "surface_effects_contour_p.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace {
std::atomic<bool> reject_large_allocation{};
std::atomic<bool> attempted_large_allocation{};

[[gnu::noinline]] void ReleaseTestAllocation(void *memory) noexcept
{
    std::free(memory);
}
} // namespace

// Test-only allocation gate: oversized masks must be rejected by geometry
// validation before asking the allocator for a coverage image.
void *operator new(std::size_t size)
{
    if (reject_large_allocation && size >= prism::wm::effect_detail::ContourMaskPixelLimit) {
        attempted_large_allocation = true;
        throw std::bad_alloc();
    }
    if (auto *memory = std::malloc(std::max(size, std::size_t{1}))) {
        return memory;
    }
    throw std::bad_alloc();
}

void *operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void *memory) noexcept
{
    ReleaseTestAllocation(memory);
}

void operator delete[](void *memory) noexcept
{
    ReleaseTestAllocation(memory);
}

void operator delete(void *memory, std::size_t) noexcept
{
    ReleaseTestAllocation(memory);
}

void operator delete[](void *memory, std::size_t) noexcept
{
    ReleaseTestAllocation(memory);
}

namespace {
using namespace prism;
using wm::effect_detail::RasterizeContourCoverage;

void Require(bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

contracts::Contour Rectangle()
{
    return {{{0, 0}, {2, 0}, {2, 2}, {0, 2}}};
}

void FractionalArea()
{
    auto contour = Rectangle();
    const auto pixels = RasterizeContourCoverage(contour, 4, 4, .25, .25);
    const std::vector<std::uint8_t> expected{143, 191, 48, 0, 191, 255, 64, 0,
                                             48,  64,  16, 0, 0,   0,   0,  0};
    Require(pixels == expected, "Fractional rectangle coverage lost subpixel area");

    std::reverse(contour.points.begin(), contour.points.end());
    Require(RasterizeContourCoverage(contour, 4, 4, .25, .25) == pixels,
            "Coverage depended on contour winding");
    for (auto &point : contour.points) {
        point.x -= 1000.75;
        point.y += 123.125;
    }
    Require(RasterizeContourCoverage(contour, 4, 4, .25, .25) == pixels,
            "Surface-local translation changed target-relative coverage");
}

void ConcaveIntervals()
{
    const contracts::Contour contour{
        {{0, 0}, {8, 0}, {8, 8}, {6, 8}, {6, 2}, {2, 2}, {2, 8}, {0, 8}}};
    contracts::ValidateContour(contour);
    const auto pixels = RasterizeContourCoverage(contour, 8, 8, 0, 0);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const bool filled = y < 2 || x < 2 || x >= 6;
            Require(pixels[std::size_t(y) * 8 + x] == (filled ? 255 : 0),
                    "Concave gap was filled as one bounding scan interval");
        }
    }
}

void PresentationCoordinates()
{
    // Valid installed rectangles may acquire non-grid coordinates under an
    // affine presentation. Coverage must consume them without re-quantization.
    const contracts::Contour contour{{{1.11, 2.22}, {5.55, 2.22}, {5.55, 7.33}, {1.11, 7.33}}};
    const auto pixels = RasterizeContourCoverage(contour, 8, 8, .27, .41);
    Require(pixels[2 * 8] == 186 && pixels[2 * 8 + 4] == 181 && pixels[2 * 8 + 5] == 0,
            "Presentation coordinates were rounded or clipped to transport grid");
    Require(pixels[0] == 93 && pixels[3 * 8 + 2] == 255,
            "Fractional presentation origin was lost during coverage preparation");
}

void Reject(const contracts::Contour &contour, int width, int height, double x = 0, double y = 0)
{
    bool rejected = false;
    try {
        RasterizeContourCoverage(contour, width, height, x, y);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    Require(rejected, "Invalid coverage geometry was accepted");
}

void InvalidAndBudget()
{
    const auto rectangle = Rectangle();
    Reject(rectangle, 0, 1);
    Reject(rectangle, 1, 0);
    Reject(rectangle, -1, 1);
    Reject(rectangle, 8193, 1);
    Reject(rectangle, 1, 8193);
    Reject(rectangle, 2, 2, std::numeric_limits<double>::quiet_NaN());
    Reject(rectangle, 2, 2, 0, std::numeric_limits<double>::infinity());
    Reject(rectangle, 2, 2, 8193);

    auto invalid = rectangle;
    invalid.points[0].x = std::numeric_limits<double>::quiet_NaN();
    Reject(invalid, 2, 2);
    invalid = rectangle;
    invalid.points[1].y = std::numeric_limits<double>::infinity();
    Reject(invalid, 2, 2);
    invalid = rectangle;
    invalid.points[0].x = 100000001;
    Reject(invalid, 2, 2);
    Reject({}, 2, 2);
    Reject({{{0, 0}, {1, 1}}}, 2, 2);
    invalid.points.assign(contracts::ContourVertexLimit + 1, {0, 0});
    Reject(invalid, 2, 2);

    attempted_large_allocation = false;
    reject_large_allocation = true;
    try {
        Reject(rectangle, 8192, 2049);
    } catch (...) {
        reject_large_allocation = false;
        throw;
    }
    reject_large_allocation = false;
    Require(!attempted_large_allocation, "Coverage budget was checked after image allocation");
}
} // namespace

int main()
{
    try {
        FractionalArea();
        ConcaveIntervals();
        PresentationCoordinates();
        InvalidAndBudget();
        std::puts("surface contour coverage geometry and allocation budget passed");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "surface contour coverage: %s\n", error.what());
        return 1;
    }
}
