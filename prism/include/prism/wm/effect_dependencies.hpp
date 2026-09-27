#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace prism::wm::effects {
struct Rect {
    double x{}, y{}, width{}, height{};
    bool operator==(const Rect&) const = default;
};
bool IsFinite(const Rect&);
bool IsEmpty(const Rect&);
bool Intersects(const Rect&, const Rect&);
Rect Intersection(const Rect&, const Rect&);
// The capture uses ceil(result / 2) texels. The guard accounts for its raster
// rounding and linear sampling; it does not reduce the existing blur padding.
Rect CaptureFootprint(double x, double y, int result_width, int result_height,
                      double guard = 2.0);
// Clip a logical rectangle to source and linearly project it to destination.
// nullopt means an unprovable mapping; a zero rectangle means no intersection.
std::optional<Rect> MapRect(const Rect&, const Rect& source, const Rect& destination);

class DamageHistory {
public:
    static constexpr std::size_t kHistoryLength = 32;
    static constexpr std::size_t kMaxRectangles = 16;
    struct QueryResult { bool changed{}, fallback{}; };
    // Empty damage does not advance the pixel revision. Excess rectangles are
    // conservatively merged. full denotes a known whole-content replacement.
    bool Record(std::span<const Rect> damage, std::uint64_t mapping_epoch = 0,
                bool full = false);
    std::uint64_t Revision() const { return revision_; }
    QueryResult Since(std::uint64_t observed_revision, const Rect& query,
                      std::uint64_t mapping_epoch = 0) const;
private:
    struct Entry {
        std::uint64_t revision{}, mapping_epoch{};
        std::array<Rect, kMaxRectangles> rectangles{};
        std::size_t count{};
        bool full{};
    };
    std::array<Entry, kHistoryLength> entries_{};
    std::uint64_t revision_{};
    std::size_t next_{}, count_{};
};

struct DependencyStamp {
    std::uint64_t identity{}, observed_revision{}, sampled_revision{}, mapping_epoch{};
    bool operator==(const DependencyStamp&) const = default;
};
struct DependencyObservation {
    DependencyStamp stamp;
    bool changed{}, fallback{}, advanced_without_damage{};
};
// Returns a candidate only. The caller publishes it after a cache hit or a
// successful complete region render; a failed render retains the old stamp.
DependencyObservation Observe(const DamageHistory&, DependencyStamp previous,
                              std::uint64_t identity, const Rect& query,
                              std::uint64_t mapping_epoch = 0);
} // namespace prism::wm::effects
